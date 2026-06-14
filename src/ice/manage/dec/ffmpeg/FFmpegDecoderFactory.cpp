#include <fmt/format.h>

#include <ice/config/config.hpp>
#include <ice/execptions/load_error.hpp>
#include <ice/manage/dec/ffmpeg/FFmpegDecoderFactory.hpp>

#include "ice/manage/AudioTrack.hpp"
#include "ice/manage/dec/ffmpeg/FFmpegDecoderInstance.hpp"

#include <algorithm>
#include <cstdint>

extern "C" {
#include <libavformat/avformat.h>
}

#define AVCALL_CHECK(func)                          \
    if ( int ret = func < 0 ) {                     \
        char errbuf[AV_ERROR_MAX_STRING_SIZE];      \
        av_strerror(ret, errbuf, sizeof(errbuf));   \
        throw ice::load_error(std::string(errbuf)); \
    }

namespace ice
{
namespace
{

/// @brief 解析探测阶段可展示的声道数。
/// @param codecParams FFmpeg 音频流参数。
/// @return 正声道数。
int probe_channel_count(const AVCodecParameters* codecParams)
{
    if ( codecParams && codecParams->ch_layout.nb_channels > 0 ) {
        return codecParams->ch_layout.nb_channels;
    }
    return std::max<int>(1,
                         static_cast<int>(ICEConfig::internal_format.channels));
}

/// @brief 解析探测阶段可展示的采样率。
/// @param codecParams FFmpeg 音频流参数。
/// @return 正采样率。
int probe_sample_rate(const AVCodecParameters* codecParams)
{
    if ( codecParams && codecParams->sample_rate > 0 ) {
        return codecParams->sample_rate;
    }
    return std::max<int>(
        1, static_cast<int>(ICEConfig::internal_format.samplerate));
}

/// @brief 将 FFmpeg duration 按指定 time_base 转换为目标采样率下的帧数。
/// @param duration FFmpeg duration 值。
/// @param timeBase duration 对应的时间基。
/// @param sampleRate 目标采样率。
/// @return 可用帧数；duration 无效时返回 0。
std::int64_t durationToFrameCount(std::int64_t duration, AVRational timeBase,
                                  int sampleRate)
{
    if ( duration <= 0 || sampleRate <= 0 ) {
        return 0;
    }
    return av_rescale_q(duration, timeBase, { 1, sampleRate });
}

/// @brief 在流级和容器级 duration 中选择更可靠的音频总帧数。
/// @param fmtCtx 已打开并完成 stream info 探测的 FFmpeg 上下文。
/// @param audioStream 目标音频流。
/// @param sampleRate 目标采样率。
/// @return 探测到的最大可用总帧数，未知时返回 0。
std::int64_t probeBestFrameCount(const AVFormatContext* fmtCtx,
                                 const AVStream* audioStream, int sampleRate)
{
    const std::int64_t streamFrames =
        audioStream
            ? durationToFrameCount(
                  audioStream->duration, audioStream->time_base, sampleRate)
            : 0;
    const std::int64_t formatFrames =
        fmtCtx ? durationToFrameCount(
                     fmtCtx->duration, { 1, AV_TIME_BASE }, sampleRate)
               : 0;
    return std::max(streamFrames, formatFrames);
}

}  // namespace

class FFmpegFormat
{
public:
    explicit FFmpegFormat(std::string_view file)
    {
        const std::string path_str(file);
        // 打开文件
        AVCALL_CHECK(
            avformat_open_input(&fmt_ctx, path_str.c_str(), nullptr, nullptr))
    }
    ~FFmpegFormat() { avformat_close_input(&fmt_ctx); }
    FFmpegFormat(const FFmpegFormat&)            = delete;
    FFmpegFormat& operator=(const FFmpegFormat&) = delete;

    inline AVFormatContext* get() const { return fmt_ctx; }

private:
    AVFormatContext* fmt_ctx{ nullptr };
};

// 探测文件元信息
bool FFmpegDecoderFactory::probe(std::string_view file_path,
                                 MediaInfo&       media_info) const
{
    const std::string path_str(file_path);
    AVFormatContext*  fmt_ctx{ nullptr };

    if ( avformat_open_input(&fmt_ctx, path_str.c_str(), nullptr, nullptr) <
         0 ) {
        return false;
    }

    auto cleanup = [&]() { avformat_close_input(&fmt_ctx); };

    if ( avformat_find_stream_info(fmt_ctx, nullptr) < 0 ) {
        cleanup();
        return false;
    }

    // find audio stream
    int audio_stream_index{ -1 };
    for ( int i = 0; i < fmt_ctx->nb_streams; i++ ) {
        // codecpar stores decoder info
        if ( fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO ) {
            audio_stream_index = i;
            break;
        }
    }
    if ( audio_stream_index == -1 ) {
        fmt::print("there\'s no audio stream in {}\n", file_path);
        media_info.format.channels = 0;
        cleanup();
        return false;
    }

    // 获取音频流的参数
    auto& audio_stream = fmt_ctx->streams[audio_stream_index];
    auto& codec_params = audio_stream->codecpar;

    media_info.format.channels =
        static_cast<uint16_t>(probe_channel_count(codec_params));
    media_info.format.samplerate =
        static_cast<uint32_t>(probe_sample_rate(codec_params));

    // 获取总采样数。部分容器的 stream duration 会短到只有首个 packet
    // 附近，需与容器级 duration 比较后取更可信的较大值。
    const std::int64_t bestFrameCount = probeBestFrameCount(
        fmt_ctx, audio_stream, static_cast<int>(media_info.format.samplerate));
    if ( bestFrameCount > 0 ) {
        media_info.frame_count = static_cast<size_t>(bestFrameCount);
    } else {
        fmt::print("file {} duration unknown\n", file_path);
        // 表示未知
        media_info.frame_count = 0;
    }

    media_info.bitrate = fmt_ctx->bit_rate;
    // 4. 从字典中获取元数据
    const AVDictionaryEntry* tag = nullptr;

    // 获取艺术家
    tag = av_dict_get(
        fmt_ctx->metadata, "artist", nullptr, AV_DICT_IGNORE_SUFFIX);
    if ( tag ) {
        media_info.artist = std::string(tag->value);
    }

    // 获取专辑
    tag =
        av_dict_get(fmt_ctx->metadata, "album", nullptr, AV_DICT_IGNORE_SUFFIX);
    if ( tag ) {
        media_info.album = std::string(tag->value);
    }

    // 获取标题
    tag =
        av_dict_get(fmt_ctx->metadata, "title", nullptr, AV_DICT_IGNORE_SUFFIX);
    if ( tag ) {
        media_info.title = std::string(tag->value);
    }

    // 查找并提取专辑封面
    for ( unsigned int i = 0; i < fmt_ctx->nb_streams; ++i ) {
        const AVStream* stream = fmt_ctx->streams[i];
        if ( stream->disposition & AV_DISPOSITION_ATTACHED_PIC ) {
            const AVPacket& packet = stream->attached_pic;
            if ( packet.size > 0 && packet.data ) {
                // 内存分配和拷贝在这里发生
                media_info.cover.size = packet.size;
                media_info.cover.data = new uint8_t[media_info.cover.size];
                memcpy(
                    media_info.cover.data, packet.data, media_info.cover.size);
                break;
            }
        }
    }
    cleanup();
    return true;
}

// 创建解码器实例
std::unique_ptr<IDecoderInstance> FFmpegDecoderFactory::create_instance(
    std::string_view file_path, const ice::AudioDataFormat& target_format) const
{
    return std::make_unique<FFmpegDecoderInstance>(file_path, target_format);
}

}  // namespace ice
