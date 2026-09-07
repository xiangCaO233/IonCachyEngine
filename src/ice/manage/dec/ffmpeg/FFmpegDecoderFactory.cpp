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

/// @brief 历史包装类使用的错误检查宏，当前 probe 不经此宏返回失败。
/// @warning 赋值右侧是比较表达式，ret 并非原始 FFmpeg 负错误码。
/// 诊断文本不能用来恢复原始错误值；修正表达式需要独立的行为验证。
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
    // 优先保留媒体自身声道数，不在元信息探测阶段强制改成混音声道数。
    if ( codecParams && codecParams->ch_layout.nb_channels > 0 ) {
        return codecParams->ch_layout.nb_channels;
    }
    // 缺失元信息时仍返回正数，避免下游按零声道准备资源。
    return std::max<int>(1,
                         static_cast<int>(ICEConfig::internal_format.channels));
}

/// @brief 解析探测阶段可展示的采样率。
/// @param codecParams FFmpeg 音频流参数。
/// @return 正采样率。
int probe_sample_rate(const AVCodecParameters* codecParams)
{
    // 容器中的有效采样率属于源音频；只有缺失时才使用内部配置回退。
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
    // 未知或无效时长不转为无符号帧数，防止负哨兵值变成超大容量。
    if ( duration <= 0 || sampleRate <= 0 ) {
        return 0;
    }
    // 使用有理数重标度，避免先转浮点秒数后再累计舍入误差。
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
    // 流与容器分别使用自己的时间基，不能直接比较原始 duration 数值。
    const std::int64_t streamFrames =
        audioStream
            ? durationToFrameCount(
                  audioStream->duration, audioStream->time_base, sampleRate)
            : 0;
    const std::int64_t formatFrames =
        fmtCtx ? durationToFrameCount(
                     fmtCtx->duration, { 1, AV_TIME_BASE }, sampleRate)
               : 0;
    // 取较大估算防止部分容器的流时长只覆盖首包，最终仍以实际解码为准。
    return std::max(streamFrames, formatFrames);
}

}  // namespace

/// @brief 历史容器句柄包装，析构关闭已经打开的输入。
/// 当前 probe 使用显式 cleanup 路径，不经此包装创建上下文。
class FFmpegFormat
{
public:
    /// @brief 将借用路径转为以零结尾的字符串后打开媒体。
    /// @warning 打开可能阻塞，错误沿用历史 AVCALL_CHECK 通道。
    explicit FFmpegFormat(std::string_view file)
    {
        const std::string path_str(file);
        // 打开文件
        AVCALL_CHECK(
            avformat_open_input(&fmt_ctx, path_str.c_str(), nullptr, nullptr))
    }
    /// @brief 关闭输入并释放容器拥有的流和字典。
    ~FFmpegFormat() { avformat_close_input(&fmt_ctx); }
    FFmpegFormat(const FFmpegFormat&)            = delete;
    FFmpegFormat& operator=(const FFmpegFormat&) = delete;

    /// @brief 借用上下文，调用方不得另行关闭或缓存到包装对象生命周期之外。
    inline AVFormatContext* get() const { return fmt_ctx; }

private:
    /// @brief 构造时打开、析构时关闭的唯一输入上下文。
    AVFormatContext* fmt_ctx{ nullptr };
};

/// @brief 探测首个音频流以及容器标签、封面和时长估算。
/// @warning 同步文件访问及内存分配，必须在资源加载线程调用。
/// 失败可能已经写入部分字段，不应将 false 结果作为完整有效元信息使用。
bool FFmpegDecoderFactory::probe(std::string_view file_path,
                                 MediaInfo&       media_info) const
{
    // string_view 不保证以零结尾，先持有副本再传给 C 接口。
    const std::string path_str(file_path);
    AVFormatContext*  fmt_ctx{ nullptr };

    if ( avformat_open_input(&fmt_ctx, path_str.c_str(), nullptr, nullptr) <
         0 ) {
        return false;
    }

    // 后续每个退出分支都必须关闭输入，不能把流或字典借用地址带出去。
    auto cleanup = [&]() { avformat_close_input(&fmt_ctx); };

    // 打开成功不代表流参数齐全，流信息探测失败也必须关闭输入。
    if ( avformat_find_stream_info(fmt_ctx, nullptr) < 0 ) {
        cleanup();
        return false;
    }

    // 选择首个音频流，与解码实例的单音频流读取策略对应。
    int audio_stream_index{ -1 };
    for ( int i = 0; i < fmt_ctx->nb_streams; i++ ) {
        // 封面可能也是流，但只有音频类型可作为播放元信息来源。
        if ( fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO ) {
            audio_stream_index = i;
            break;
        }
    }
    // 没有音频流的媒体不作为音轨接受；声道零用于明确此次探测失败。
    if ( audio_stream_index == -1 ) {
        fmt::print("there\'s no audio stream in {}\n", file_path);
        media_info.format.channels = 0;
        cleanup();
        return false;
    }

    // 以下引用仅在 cleanup 前有效，公开结果保存数值及字符串副本。
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
    // 只把有效正值写入 size_t，未知时长保留零这一约定。
    if ( bestFrameCount > 0 ) {
        media_info.frame_count = static_cast<size_t>(bestFrameCount);
    } else {
        fmt::print("file {} duration unknown\n", file_path);
        // 表示未知
        // 零是未能估算时长，不证明实际音频一定为空。
        media_info.frame_count = 0;
    }

    // 保存容器报告的码率，不用采样率和浮点 PCM 位宽推算压缩码率。
    media_info.bitrate = fmt_ctx->bit_rate;
    // 只覆盖实际存在的字典项，缺失标签不会自动清空旧值。
    // 调用方应使用新的 MediaInfo，避免复用时混入上一媒体的标签。
    const AVDictionaryEntry* tag = nullptr;

    // 字符串深拷贝必须在容器关闭前完成，不保留字典条目的 char 指针。
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

    // 只提取嵌入的压缩封面包，不在音频探测阶段解码图像像素。
    // 查找并提取专辑封面
    for ( unsigned int i = 0; i < fmt_ctx->nb_streams; ++i ) {
        const AVStream* stream = fmt_ctx->streams[i];
        if ( stream->disposition & AV_DISPOSITION_ATTACHED_PIC ) {
            const AVPacket& packet = stream->attached_pic;
            // 跳过空封面流，继续寻找第一个有实际字节的附图。
            if ( packet.size > 0 && packet.data ) {
                // 深拷贝后封面归 MediaInfo 管理，输入关闭不会使其失效。
                // 目标应为空封面；本路径不负责回收调用方此前填入的数组。
                media_info.cover.size = packet.size;
                media_info.cover.data = new uint8_t[media_info.cover.size];
                memcpy(
                    media_info.cover.data, packet.data, media_info.cover.size);
                // 找到首个有效封面即停止，不将多个附图合并为一个字节流。
                break;
            }
        }
    }
    // 所有需要的数据都已复制完成，关闭输入不影响返回对象。
    cleanup();
    return true;
}

/// @brief 为每次解码建立独立上下文，避免多个音轨共享可变解码状态。
/// @warning 构造会打开媒体并分配转换资源，失败沿用已有异常语义。
std::unique_ptr<IDecoderInstance> FFmpegDecoderFactory::create_instance(
    std::string_view file_path, const ice::AudioDataFormat& target_format) const
{
    // 探测与实际解码分开打开文件，调用方应允许期间媒体被移走或改变。
    return std::make_unique<FFmpegDecoderInstance>(file_path, target_format);
}

}  // namespace ice
