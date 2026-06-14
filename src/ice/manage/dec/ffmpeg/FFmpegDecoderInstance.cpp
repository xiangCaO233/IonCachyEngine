#include <fmt/base.h>
#include <fmt/format.h>

#include <ice/manage/dec/ffmpeg/FFmpegDecoderFactory.hpp>
#include <ice/manage/dec/ffmpeg/FFmpegDecoderInstance.hpp>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>

#include "ice/execptions/load_error.hpp"
#include "ice/manage/AudioBuffer.hpp"
#include "ice/manage/AudioFormat.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/codec.h>
#include <libavcodec/defs.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libswresample/swresample.h>
}

namespace ice
{
namespace
{

/// @brief 将 FFmpeg 错误码转换为可读文本。
/// @param code FFmpeg 返回的错误码。
/// @return 错误信息字符串。
std::string ffmpeg_error_string(int code)
{
    char errbuf[AV_ERROR_MAX_STRING_SIZE] = { 0 };
    av_strerror(code, errbuf, sizeof(errbuf));
    return std::string(errbuf);
}

/// @brief 检查 FFmpeg 调用返回值，失败时抛出项目既有 load_error。
/// @param ret FFmpeg 调用返回值。
void check_av_call(int ret)
{
    if ( ret < 0 ) {
        throw ice::load_error(ffmpeg_error_string(ret));
    }
}

/// @brief 检查 FFmpeg 调用返回值，失败时打印错误并返回 false。
/// @param ret FFmpeg 调用返回值。
/// @return 成功时返回 true。
bool check_av_call_ret_bool(int ret)
{
    if ( ret < 0 ) {
        fmt::print("averr: {}", ffmpeg_error_string(ret));
        return false;
    }
    return true;
}

/// @brief 判断解码错误是否可以按播放器容错策略跳过。
/// @param ret FFmpeg 返回的错误码。
/// @return 可以跳过并继续读取后续 packet 时返回 true。
bool is_recoverable_decode_error(int ret)
{
    return ret == AVERROR_INVALIDDATA;
}

/// @brief 判断 FFmpeg 声道布局是否可直接交给重采样器。
/// @param layout 待检查布局。
/// @return 布局有效时返回 true。
bool is_usable_channel_layout(const AVChannelLayout& layout)
{
    return layout.nb_channels > 0 && layout.order != AV_CHANNEL_ORDER_UNSPEC &&
           av_channel_layout_check(&layout) > 0;
}

/// @brief 解析源音频声道数，用于布局缺失时兜底。
/// @param codec_ctx 已打开的解码器上下文。
/// @param codec_params 音频流参数。
/// @param target_format 引擎目标格式。
/// @return 可用于解码的正声道数。
int resolve_source_channel_count(const AVCodecContext*       codec_ctx,
                                 const AVCodecParameters*    codec_params,
                                 const ice::AudioDataFormat& target_format)
{
    if ( codec_ctx && codec_ctx->ch_layout.nb_channels > 0 ) {
        return codec_ctx->ch_layout.nb_channels;
    }
    if ( codec_params && codec_params->ch_layout.nb_channels > 0 ) {
        return codec_params->ch_layout.nb_channels;
    }
    return std::max<int>(1, static_cast<int>(target_format.channels));
}

/// @brief 解析源音频采样率，用于少数容器元信息缺失时兜底。
/// @param codec_ctx 已打开的解码器上下文。
/// @param codec_params 音频流参数。
/// @param target_format 引擎目标格式。
/// @return 可用于重采样的正采样率。
int resolve_source_sample_rate(const AVCodecContext*       codec_ctx,
                               const AVCodecParameters*    codec_params,
                               const ice::AudioDataFormat& target_format)
{
    if ( codec_ctx && codec_ctx->sample_rate > 0 ) {
        return codec_ctx->sample_rate;
    }
    if ( codec_params && codec_params->sample_rate > 0 ) {
        return codec_params->sample_rate;
    }
    return std::max<int>(1, static_cast<int>(target_format.samplerate));
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

/// @brief 复制有效源声道布局，缺失或未指定时生成默认布局。
/// @param output 输出布局。
/// @param codec_ctx 已打开的解码器上下文。
/// @param codec_params 音频流参数。
/// @param target_format 引擎目标格式。
/// @return FFmpeg 错误码；成功为非负。
int make_source_channel_layout(AVChannelLayout*            output,
                               const AVCodecContext*       codec_ctx,
                               const AVCodecParameters*    codec_params,
                               const ice::AudioDataFormat& target_format)
{
    if ( !output ) {
        return AVERROR(EINVAL);
    }

    if ( codec_ctx && is_usable_channel_layout(codec_ctx->ch_layout) ) {
        return av_channel_layout_copy(output, &codec_ctx->ch_layout);
    }
    if ( codec_params && is_usable_channel_layout(codec_params->ch_layout) ) {
        return av_channel_layout_copy(output, &codec_params->ch_layout);
    }

    const int channel_count =
        resolve_source_channel_count(codec_ctx, codec_params, target_format);
    av_channel_layout_default(output, channel_count);
    return output->nb_channels > 0 ? 0 : AVERROR(EINVAL);
}

#define AVCALL_CHECK(func) check_av_call((func));
#define AVCALL_CHECKRETB(func)               \
    if ( !check_av_call_ret_bool((func)) ) { \
        return false;                        \
    }

}  // namespace

// 完整定义
class FFmpegDecoder
{
public:
    explicit FFmpegDecoder(std::string_view            file_path,
                           const ice::AudioDataFormat& target_format)
    {
        const std::string path_str(file_path);
        // 打开文件
        AVCALL_CHECK(
            avformat_open_input(&avfmt_ctx, path_str.c_str(), nullptr, nullptr))

        // 查找流信息(艾斯比mp3)
        AVCALL_CHECK(avformat_find_stream_info(avfmt_ctx, nullptr))

        // 查找音频流
        for ( unsigned int i = 0; i < avfmt_ctx->nb_streams; i++ ) {
            // codecpar 存储了解码器信息
            if ( avfmt_ctx->streams[i]->codecpar->codec_type ==
                 AVMEDIA_TYPE_AUDIO ) {
                stream_index = static_cast<int>(i);
                break;
            }
        }
        if ( stream_index == -1 ) {
            fmt::print("there\'s no audio stream");
            ice_format.channels = 0;
            throw ice::load_error("find stream failed");
        }

        // 获取音频流的参数
        auto& audio_stream = avfmt_ctx->streams[stream_index];
        auto& codec_params = audio_stream->codecpar;

        ice_format = target_format;

        // 获取总采样数。部分容器的 stream duration 会短到只有首个 packet
        // 附近，需与容器级 duration 比较后取更可信的较大值。
        const std::int64_t bestFrameCount = probeBestFrameCount(
            avfmt_ctx, audio_stream, static_cast<int>(ice_format.samplerate));
        if ( bestFrameCount > 0 ) {
            total_frames = static_cast<size_t>(bestFrameCount);
        } else {
            fmt::print("file {} duration unknown", file_path);
            // 未知放0
            total_frames = 0;
        }

        // 查找编解码器
        avcodec = avcodec_find_decoder(codec_params->codec_id);
        if ( !avcodec ) {
            throw ice::load_error("no decoder find.");
        }

        // 分配编解码器
        avcodec_ctx = avcodec_alloc_context3(avcodec);
        if ( !avcodec_ctx ) {
            throw ice::load_error("decoder_context_alloc failed.");
        }

        // 将流的参数拷贝到解码器上下文中
        // 它告诉解码器要处理的数据的采样率,声道,格式等信息
        AVCALL_CHECK(avcodec_parameters_to_context(avcodec_ctx, codec_params))

        avcodec_ctx->err_recognition |= AV_EF_IGNORE_ERR;
        avcodec_ctx->flags |= AV_CODEC_FLAG_OUTPUT_CORRUPT;

        // 打开解码器
        AVCALL_CHECK(avcodec_open2(avcodec_ctx, avcodec, nullptr))

        m_sourceSampleRate =
            resolve_source_sample_rate(avcodec_ctx, codec_params, ice_format);
        AVChannelLayout sourceLayout{};
        AVCALL_CHECK(make_source_channel_layout(
            &sourceLayout, avcodec_ctx, codec_params, ice_format))
        m_duplicateMonoToTargetChannels =
            sourceLayout.nb_channels == 1 && ice_format.channels > 1;
        // 分配内存并循环解码
        // 用于存放从文件中读取的压缩数据包
        avpacket = av_packet_alloc();
        // 用于存放解码后的原始 PCM 数据帧
        avframe = av_frame_alloc();

        // 初始化重采样器 SwrContext
        // This function does not require *ps to be allocated with
        // swr_alloc(). On the
        // other hand, swr_alloc() can use swr_alloc_set_opts2() to set
        // the parameters
        // on the allocated context.
        // 初始化重采样器 SwrContext
        AVChannelLayout tgtch_layout{};
        av_channel_layout_default(&tgtch_layout, target_format.channels);

        const int swrAllocRet = swr_alloc_set_opts2(
            // @param ps
            // Pointer to an existing Swr context if available,
            // or to NULL if not.
            // On success, *ps will be set to the allocated context.
            //
            &swr_ctx,
            &tgtch_layout,       // 目标声道布局
            AV_SAMPLE_FMT_FLTP,  // 目标样本格式固定为planner-float(32位浮点数，平面)
            target_format.samplerate,  // 目标采样率
            &sourceLayout,             // 源声道布局
            avcodec_ctx->sample_fmt,   // 源样本格式
            m_sourceSampleRate,        // 源采样率
            0,                         // 日志偏移
            nullptr                    // 日志上下文
        );
        av_channel_layout_uninit(&tgtch_layout);
        av_channel_layout_uninit(&sourceLayout);
        AVCALL_CHECK(swrAllocRet)

        if ( !swr_ctx ) {
            throw ice::load_error("swr_alloc_set_opts failed.");
        }

        // 初始化重采样器上下文
        AVCALL_CHECK(swr_init(swr_ctx))

        // 初始化转换缓冲区
        const size_t max_frames_per_avframe = 2048;
        conversion_buffer.resize(ice_format, max_frames_per_avframe);
    }
    ~FFmpegDecoder()
    {
        // 释放
        av_frame_free(&avframe);
        av_packet_free(&avpacket);
        avcodec_free_context(&avcodec_ctx);
        avformat_close_input(&avfmt_ctx);
        if ( swr_ctx ) {
            swr_free(&swr_ctx);
        }
    }
    FFmpegDecoder(const FFmpegDecoder&)            = delete;
    FFmpegDecoder& operator=(const FFmpegDecoder&) = delete;

    // 移动指针到指定帧位置
    bool seek_to_frame(size_t frame_offset)
    {
        // 将帧偏移量转换回FFmpeg的时间基单位
        int64_t timestamp =
            av_rescale_q(frame_offset,
                         { 1, static_cast<int>(ice_format.samplerate) },
                         avfmt_ctx->streams[stream_index]->time_base);

        // 执行寻道操作
        // AVSEEK_FLAG_BACKWARD 保证我们能seek到请求的关键帧或其之前最近的关键帧
        AVCALL_CHECKRETB(av_seek_frame(
            avfmt_ctx, stream_index, timestamp, AVSEEK_FLAG_BACKWARD))

        // 寻道后解码器内部的状态可能是不一致的,需要刷新
        avcodec_flush_buffers(avcodec_ctx);
        av_packet_unref(avpacket);
        av_frame_unref(avframe);
        conversion_buffer_remains   = 0;
        conversion_buffer_offset    = 0;
        m_pendingPacketTargetFrames = 0;
        if ( swr_ctx ) {
            swr_close(swr_ctx);
            AVCALL_CHECKRETB(swr_init(swr_ctx))
        }
        return true;
    }

    /// @brief 解码数据并在坏包/坏帧上保留已解出的音频。
    /// @param buffer 输出缓冲区，按声道平面排列。
    /// @param chunksize 本次请求的目标帧数。
    /// @return 实际写入的目标帧数。
    size_t decode(float** buffer, size_t chunksize)
    {
        size_t frames_decoded_total = 0;

        while ( frames_decoded_total < chunksize ) {
            // 优先处理上一次遗留的数据
            if ( conversion_buffer_remains > 0 ) {
                size_t frames_to_copy =
                    std::min(chunksize - frames_decoded_total,
                             conversion_buffer_remains);

                for ( uint16_t ch = 0; ch < ice_format.channels; ++ch ) {
                    const float* src  = conversion_buffer.raw_ptrs()[ch] +
                                        conversion_buffer_offset;
                    float*       dest = buffer[ch] + frames_decoded_total;
                    std::memcpy(dest, src, frames_to_copy * sizeof(float));
                }

                frames_decoded_total += frames_to_copy;
                conversion_buffer_remains -= frames_to_copy;
                conversion_buffer_offset += frames_to_copy;

                if ( frames_decoded_total >= chunksize ) break;
            }

            // 如果遗留数据已用完，重置偏移
            conversion_buffer_offset = 0;

            // 从解码器获取一个新的原生帧
            int ret = avcodec_receive_frame(avcodec_ctx, avframe);

            if ( ret == 0 ) {
                // 将整个原生帧转换到的内部缓冲区
                const int output_capacity =
                    ensure_conversion_buffer_capacity(avframe->nb_samples);
                int converted_count =
                    swr_convert(swr_ctx,
                                (uint8_t**)conversion_buffer.raw_ptrs(),
                                output_capacity,
                                (const uint8_t**)avframe->data,
                                avframe->nb_samples);

                if ( converted_count > 0 ) {
                    duplicateMonoChannelToTargetChannels(
                        static_cast<size_t>(converted_count));
                    // 更新我们内部缓冲区的剩余帧数
                    conversion_buffer_remains = converted_count;
                }
                av_frame_unref(avframe);
                m_pendingPacketTargetFrames = 0;

                continue;
            } else if ( ret == AVERROR(EAGAIN) ) {
                // 发送新Packet
                av_packet_unref(avpacket);
                int read_ret = av_read_frame(avfmt_ctx, avpacket);
                if ( read_ret == AVERROR_EOF ) {
                    // 发送空pack
                    m_pendingPacketTargetFrames = 0;
                    const int flushRet =
                        avcodec_send_packet(avcodec_ctx, nullptr);
                    if ( flushRet < 0 && flushRet != AVERROR_EOF ) {
                        break;
                    }
                    // 不需要continue，下一次while循环会处理解码器的EOF
                } else if ( read_ret < 0 ) {
                    // 读文件出错，终止
                    break;
                } else if ( avpacket->stream_index == stream_index ) {
                    m_pendingPacketTargetFrames =
                        estimatePacketTargetFrames(avpacket);
                    const int sendRet =
                        avcodec_send_packet(avcodec_ctx, avpacket);
                    if ( sendRet < 0 ) {
                        if ( !recoverFromDecodeError(sendRet) ) {
                            break;
                        }
                    }
                }
            } else if ( ret == AVERROR_EOF ) {
                // 解码器已被完全清空，没有更多帧了
                // 冲洗(flush)重采样器，获取它内部缓冲的最后几帧
                int flushed_count;
                do {
                    // 先冲洗到临时缓冲区
                    const int output_capacity =
                        ensure_conversion_buffer_capacity(0);
                    flushed_count =
                        swr_convert(swr_ctx,
                                    (uint8_t**)conversion_buffer.raw_ptrs(),
                                    output_capacity,
                                    nullptr,
                                    0);
                    if ( flushed_count > 0 ) {
                        duplicateMonoChannelToTargetChannels(
                            static_cast<size_t>(flushed_count));
                        // 再从临时缓冲区拷贝到最终位置
                        size_t frames_needed = chunksize - frames_decoded_total;
                        size_t frames_to_copy =
                            std::min((size_t)flushed_count, frames_needed);
                        for ( uint16_t ch = 0; ch < ice_format.channels;
                              ++ch ) {
                            const float* src =
                                conversion_buffer.raw_ptrs()[ch] +
                                conversion_buffer_offset;
                            float* dest = buffer[ch] + frames_decoded_total;
                            std::memcpy(
                                dest, src, frames_to_copy * sizeof(float));
                        }
                        frames_decoded_total += frames_to_copy;
                    }
                } while ( flushed_count > 0 );
                // 到达文件末尾，跳出整个while循环
                break;
            } else if ( recoverFromDecodeError(ret) ) {
                continue;
            } else {
                // 发生了一个真正的解码错误
                break;
            }
        }
        return frames_decoded_total;
    }

    inline const AudioDataFormat& iceformat() const { return ice_format; }

    inline size_t frames() const { return total_frames; }

private:
    /// @brief 估算当前 packet 在目标采样率下占用的帧数。
    /// @param packet 由 FFmpeg 读取到的压缩音频包。
    /// @return packet 时长对应的目标帧数；未知时返回 0。
    size_t estimatePacketTargetFrames(const AVPacket* packet) const
    {
        if ( !packet || packet->duration <= 0 || stream_index < 0 ||
             ice_format.samplerate == 0 ) {
            return 0;
        }

        const AVRational targetTimeBase{
            1, static_cast<int>(ice_format.samplerate)
        };
        const int64_t targetFrames =
            av_rescale_q(packet->duration,
                         avfmt_ctx->streams[stream_index]->time_base,
                         targetTimeBase);
        return targetFrames > 0 ? static_cast<size_t>(targetFrames) : 0;
    }

    /// @brief 对可恢复解码错误执行跳过和静音补偿。
    /// @param ret FFmpeg 返回的错误码。
    /// @return 错误已处理并可以继续解码时返回 true。
    bool recoverFromDecodeError(int ret)
    {
        if ( !is_recoverable_decode_error(ret) ) {
            return false;
        }

        queueRecoveredSilence(m_pendingPacketTargetFrames);
        m_pendingPacketTargetFrames = 0;
        av_frame_unref(avframe);
        avcodec_flush_buffers(avcodec_ctx);
        return true;
    }

    /// @brief 将损坏 packet 的时长补成静音，保持时间轴尽量不漂移。
    /// @param frame_count 需要补偿的目标帧数。
    void queueRecoveredSilence(size_t frame_count)
    {
        if ( frame_count == 0 || ice_format.channels == 0 ) {
            return;
        }

        if ( frame_count > conversion_buffer.num_frames() ) {
            conversion_buffer.resize(ice_format, frame_count);
        }

        for ( uint16_t ch = 0; ch < ice_format.channels; ++ch ) {
            std::fill_n(conversion_buffer.raw_ptrs()[ch], frame_count, 0.0F);
        }

        conversion_buffer_offset  = 0;
        conversion_buffer_remains = frame_count;
    }

    /// @brief 确保重采样输出缓冲能容纳当前输入可能产生的所有帧。
    /// @param input_sample_count 即将送入 swr_convert 的输入采样帧数。
    /// @return 可传给 swr_convert 的输出容量。
    int ensure_conversion_buffer_capacity(int input_sample_count)
    {
        int required =
            swr_ctx ? swr_get_out_samples(swr_ctx, input_sample_count) : 0;
        if ( required <= 0 ) {
            required = static_cast<int>(conversion_buffer.num_frames());
        }
        if ( required <= 0 ) {
            required = 1;
        }

        const auto required_frames = static_cast<size_t>(required);
        if ( required_frames > conversion_buffer.num_frames() ) {
            conversion_buffer.resize(ice_format, required_frames);
        }
        return static_cast<int>(conversion_buffer.num_frames());
    }

    /// @brief 将单声道转换结果复制到所有目标声道，保证 mono
    /// 文件以居中立体声输出。
    void duplicateMonoChannelToTargetChannels(size_t frame_count)
    {
        if ( !m_duplicateMonoToTargetChannels || frame_count == 0 ||
             ice_format.channels < 2 ) {
            return;
        }

        float**      channel_ptrs = conversion_buffer.raw_ptrs();
        const float* mono_src     = channel_ptrs[0];
        for ( uint16_t ch = 1; ch < ice_format.channels; ++ch ) {
            std::memcpy(
                channel_ptrs[ch], mono_src, frame_count * sizeof(float));
        }
    }

    AVFormatContext* avfmt_ctx{ nullptr };
    AVCodecContext*  avcodec_ctx{ nullptr };
    const AVCodec*   avcodec{ nullptr };
    AVPacket*        avpacket{ nullptr };
    AVFrame*         avframe{ nullptr };
    SwrContext*      swr_ctx{ nullptr };
    AudioDataFormat  ice_format;
    AudioBuffer      conversion_buffer;
    /// @brief 标记当前源文件是否需要在解码后复制单声道到目标多声道。
    bool m_duplicateMonoToTargetChannels{ false };
    /// @brief 最近送入解码器的 packet 在目标采样率下的估算帧数。
    size_t m_pendingPacketTargetFrames{ 0 };
    // 新增：记录上一次转换后，在 conversion_buffer 中还剩下多少帧
    size_t conversion_buffer_remains = 0;
    // 新增：记录上一次转换后，我们从缓冲区的哪个位置开始拷贝的
    size_t conversion_buffer_offset = 0;
    /// @brief 源音频采样率，供重采样和异常元信息兜底使用。
    int m_sourceSampleRate{ 0 };

    size_t total_frames{ 0 };
    int    stream_index{ -1 };
};

FFmpegDecoderInstance::FFmpegDecoderInstance(
    std::string_view file_path, const ice::AudioDataFormat& target_format)
{
    // 初始化解码器资源
    ffimpl = std::make_unique<FFmpegDecoder>(file_path, target_format);
}
FFmpegDecoderInstance::~FFmpegDecoderInstance() = default;

// 定位帧位置
bool FFmpegDecoderInstance::seek(size_t pos)
{
    return ffimpl->seek_to_frame(pos);
}

// 读取一块数据
size_t FFmpegDecoderInstance::read(float** buffer, size_t chunksize)
{
    return ffimpl->decode(buffer, chunksize);
}

const AudioDataFormat& FFmpegDecoderInstance::get_source_format() const
{
    return ffimpl->iceformat();
}
size_t FFmpegDecoderInstance::get_source_total_frames() const
{
    return ffimpl->frames();
}

}  // namespace ice
