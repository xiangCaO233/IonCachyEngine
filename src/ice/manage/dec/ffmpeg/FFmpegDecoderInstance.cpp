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
/// @warning 构造路径沿用历史异常机制，失败不会自动释放未构造完成对象的裸句柄。
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
/// 仅损坏数据属于容错范围，内存、IO 和其他错误不能当作普通坏包吞掉。
bool is_recoverable_decode_error(int ret)
{
    return ret == AVERROR_INVALIDDATA;
}

/// @brief 判断 FFmpeg 声道布局是否可直接交给重采样器。
/// @param layout 待检查布局。
/// @return 布局有效时返回 true。
/// 只有声道数而没有顺序的布局仍需补默认映射，不能直接交给混音矩阵。
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
    // 已打开的解码器可能补全容器缺失字段，优先使用其解析后的声道数。
    if ( codec_ctx && codec_ctx->ch_layout.nb_channels > 0 ) {
        return codec_ctx->ch_layout.nb_channels;
    }
    if ( codec_params && codec_params->ch_layout.nb_channels > 0 ) {
        return codec_params->ch_layout.nb_channels;
    }
    // 最后回退只保证正数，不代表已从媒体确认真实声道布局。
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
    // 源采样率与输出采样率分开；仅在源信息均缺失时才借用目标配置。
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
/// 时间基转换结果按目标采样帧计数，不能再乘声道数。
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
    // 较大值缓解流时长仅覆盖首包的容器问题，但不是实际解码长度的上界保证。
    return std::max(streamFrames, formatFrames);
}

/// @brief 复制有效源声道布局，缺失或未指定时生成默认布局。
/// @param output 输出布局。
/// @param codec_ctx 已打开的解码器上下文。
/// @param codec_params 音频流参数。
/// @param target_format 引擎目标格式。
/// @return FFmpeg 错误码；成功为非负。
/// @pre output 指向零初始化或已经释放旧布局的存储，调用方负责最终 uninit。
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

    // 默认布局是一种缺失元信息的策略，不从声道数推断特殊定制声道顺序。
    const int channel_count =
        resolve_source_channel_count(codec_ctx, codec_params, target_format);
    av_channel_layout_default(output, channel_count);
    return output->nb_channels > 0 ? 0 : AVERROR(EINVAL);
}

// 构造失败和运行期失败沿用不同通道，宏均只求值一次以保留原错误码。
#define AVCALL_CHECK(func) check_av_call((func));
#define AVCALL_CHECKRETB(func)               \
    if ( !check_av_call_ret_bool((func)) ) { \
        return false;                        \
    }

}  // namespace

/// @brief 独占一个媒体输入、解码器和重采样器，并缓存尚未交付的目标 PCM。
/// 所有方法由调用方串行使用，内部没有保护 seek/read 的锁。
/// @warning 文件解码可能分配、阻塞或走历史异常路径，不属于音频设备实时回调。
class FFmpegDecoder
{
public:
    /// @brief 选择首个音频流并准备平面浮点目标格式。
    /// @param file_path 媒体路径视图，构造期间复制为零结尾字符串。
    /// @param target_format 输出格式，采样率与声道数须有效且保持稳定。
    /// @warning 构造中部分失败仍依靠异常退出，已取得的裸资源缺少局部回滚保护。
    explicit FFmpegDecoder(std::string_view            file_path,
                           const ice::AudioDataFormat& target_format)
    {
        const std::string path_str(file_path);
        // C 接口需要零结尾，局部字符串至少存活到输入打开完成。
        AVCALL_CHECK(
            avformat_open_input(&avfmt_ctx, path_str.c_str(), nullptr, nullptr))

        // 探测补全流参数与时长，不能只依赖容器初始头部信息。
        AVCALL_CHECK(avformat_find_stream_info(avfmt_ctx, nullptr))

        // 保持容器顺序选择第一个音频流，不按语言或默认 disposition 重排。
        for ( unsigned int i = 0; i < avfmt_ctx->nb_streams; i++ ) {
            // 视频和附件流不参与 PCM 解码，后续读包还须按同一索引过滤。
            if ( avfmt_ctx->streams[i]->codecpar->codec_type ==
                 AVMEDIA_TYPE_AUDIO ) {
                stream_index = static_cast<int>(i);
                break;
            }
        }
        if ( stream_index == -1 ) {
            // 缺少音频流属于打开失败，不能创建仅报告零时长的可用解码实例。
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
            // 零是未知长度哨兵，不表示 read 一定没有可读音频。
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
        // 请求尽量产出可恢复的帧；真正返回 INVALIDDATA 时另行补静音。
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
        // 单声道经重采样后复制首通道，使全部目标声道保持一致的居中内容。
        // 用于存放从文件中读取的压缩数据包
        avpacket = av_packet_alloc();
        // 用于存放解码后的原始 PCM 数据帧
        avframe = av_frame_alloc();
        // 这两个历史分配调用未逐一检查空值，不能据构造路径宣称内存失败完备。

        // 两个临时布局只用于配置重采样器，配置调用后必须各自释放内部存储。
        AVChannelLayout tgtch_layout{};
        av_channel_layout_default(&tgtch_layout, target_format.channels);

        const int swrAllocRet = swr_alloc_set_opts2(
            // 空句柄由配置函数分配，输出约定为各声道独立的 32 位浮点平面。
            &swr_ctx,
            &tgtch_layout,             // 目标声道布局
            AV_SAMPLE_FMT_FLTP,        // 目标平面浮点样本格式
            target_format.samplerate,  // 目标采样率
            &sourceLayout,             // 源声道布局
            avcodec_ctx->sample_fmt,   // 源样本格式
            m_sourceSampleRate,        // 源采样率
            0,                         // 日志偏移
            nullptr                    // 日志上下文
        );
        av_channel_layout_uninit(&tgtch_layout);
        av_channel_layout_uninit(&sourceLayout);
        // 先释放临时布局再传播配置错误，避免本阶段的布局存储随异常遗留。
        AVCALL_CHECK(swrAllocRet)

        if ( !swr_ctx ) {
            throw ice::load_error("swr_alloc_set_opts failed.");
        }

        // 初始化重采样器上下文
        AVCALL_CHECK(swr_init(swr_ctx))
        // 分配和初始化是两个阶段；非空上下文不代表重采样配置已可执行。

        // 初始容量只是常见帧长，实际解码帧与重采样延迟可能触发后续扩容。
        const size_t max_frames_per_avframe = 2048;
        conversion_buffer.resize(ice_format, max_frames_per_avframe);
    }
    /// @brief 释放已完整构造实例持有的媒体资源，不输出或补齐剩余 PCM。
    /// @warning 析构涉及库资源回收，须在停止所有读取后于非实时侧调用。
    ~FFmpegDecoder()
    {
        // 包与帧先解除对媒体数据的引用，再销毁编解码和输入上下文。
        av_frame_free(&avframe);
        av_packet_free(&avpacket);
        avcodec_free_context(&avcodec_ctx);
        avformat_close_input(&avfmt_ctx);
        if ( swr_ctx ) {
            swr_free(&swr_ctx);
        }
    }
    /// @brief 禁止复制裸句柄所有权，避免同一 FFmpeg 资源被重复释放。
    FFmpegDecoder(const FFmpegDecoder&) = delete;
    /// @brief 禁止赋值覆盖已有媒体状态和未消费的 PCM。
    FFmpegDecoder& operator=(const FFmpegDecoder&) = delete;

    /// @brief 按目标帧偏移请求向后寻道，并清空旧的解码及重采样余量。
    /// @param frame_offset 目标采样率下的帧位置，不是源包索引。
    /// @return 寻道与重采样器重启成功时返回 true。
    /// 当前流程不按时间戳裁去关键帧到目标位置的前导采样，非逐样本精确定位。
    /// @warning 低频解码工作线程操作，可能进行文件 IO、重建或错误输出。
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

        // 成功寻道后丢弃旧位置的延迟帧、包引用及目标 PCM，避免新旧位置串音。
        avcodec_flush_buffers(avcodec_ctx);
        av_packet_unref(avpacket);
        av_frame_unref(avframe);
        conversion_buffer_remains   = 0;
        conversion_buffer_offset    = 0;
        m_pendingPacketTargetFrames = 0;
        if ( swr_ctx ) {
            // 清除滤波延迟；重启失败时输入位置已经改变，调用方应处理 false。
            swr_close(swr_ctx);
            AVCALL_CHECKRETB(swr_init(swr_ctx))
        }
        return true;
    }

    /// @brief 解码数据并在坏包/坏帧上保留已解出的音频。
    /// @param buffer 输出缓冲区，按声道平面排列。
    /// @param chunksize 本次请求的目标帧数。
    /// @return 实际写入的目标帧数。
    /// 零长度请求不拉取包，不改变解码位置或已有转换余量。
    /// @pre 每个目标声道平面可写 chunksize 个 float，同一实例无并发 seek。
    /// @warning
    /// 工作线程按需调用，包含文件读取、缓存扩容和错误恢复，不可搬入设备回调。
    size_t decode(float** buffer, size_t chunksize)
    {
        size_t frames_decoded_total = 0;

        while ( frames_decoded_total < chunksize ) {
            // 先交付转换余量，后续 receive 不得覆盖尚未消费的内部 PCM。
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
                // 拷贝数量以输出请求和缓存余量的较小者为准，避免越过调用方平面。
                conversion_buffer_remains -= frames_to_copy;
                conversion_buffer_offset += frames_to_copy;
                // 已写帧、余量和偏移同步推进，下一次 read
                // 从同一块的剩余位置接续。

                if ( frames_decoded_total >= chunksize ) break;
            }

            // 如果遗留数据已用完，重置偏移
            conversion_buffer_offset = 0;

            // 从解码器获取一个新的原生帧
            int ret = avcodec_receive_frame(avcodec_ctx, avframe);

            if ( ret == 0 ) {
                // 容量按当前输入加重采样延迟估算，不能把原生帧数直接当输出上限。
                const int output_capacity =
                    ensure_conversion_buffer_capacity(avframe->nb_samples);
                int converted_count =
                    swr_convert(swr_ctx,
                                (uint8_t**)conversion_buffer.raw_ptrs(),
                                output_capacity,
                                (const uint8_t**)avframe->data,
                                avframe->nb_samples);

                // 零输出可能只是滤波器积累输入；当前负值也被跳过而未单独报告。
                if ( converted_count > 0 ) {
                    duplicateMonoChannelToTargetChannels(
                        static_cast<size_t>(converted_count));
                    // 更新我们内部缓冲区的剩余帧数
                    conversion_buffer_remains = converted_count;
                }
                av_frame_unref(avframe);
                // 本包已经产出一帧，清除坏包时长估计，避免后续错误重复补偿。
                m_pendingPacketTargetFrames = 0;

                continue;
            } else if ( ret == AVERROR(EAGAIN) ) {
                // receive 需要新输入时才读包，优先取出解码器已经缓冲的帧。
                av_packet_unref(avpacket);
                int read_ret = av_read_frame(avfmt_ctx, avpacket);
                if ( read_ret == AVERROR_EOF ) {
                    // 容器 EOF 只结束输入，空包使解码器交付仍在内部延迟的帧。
                    m_pendingPacketTargetFrames = 0;
                    const int flushRet =
                        avcodec_send_packet(avcodec_ctx, nullptr);
                    if ( flushRet < 0 && flushRet != AVERROR_EOF ) {
                        // 进入排尾失败时保留已交付前缀，以短读返回给上层。
                        break;
                    }
                    // 不需要continue，下一次while循环会处理解码器的EOF
                } else if ( read_ret < 0 ) {
                    // 读文件出错，终止
                    break;
                } else if ( avpacket->stream_index == stream_index ) {
                    // 视频和其他音频流包留到下一轮 unref，只送选中的音频流。
                    m_pendingPacketTargetFrames =
                        estimatePacketTargetFrames(avpacket);
                    const int sendRet =
                        avcodec_send_packet(avcodec_ctx, avpacket);
                    if ( sendRet < 0 ) {
                        // send
                        // 失败只有损坏数据走恢复；其他错误中止当前读取请求。
                        if ( !recoverFromDecodeError(sendRet) ) {
                            break;
                        }
                    }
                }
            } else if ( ret == AVERROR_EOF ) {
                // 解码器 EOF 后还有重采样器滤波延迟，需用空输入继续取尾部。
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
                        // 当前只拷贝请求剩余量，未把多余 flush
                        // 帧登记为下次余量。
                        // 小块读取遇到长尾时可能丢尾，不能按此路径承诺完整尾部交付。
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
                // 循环以重采样器无正输出结束，即便本次请求空间已用尽也继续排空。
                // 到达文件末尾，跳出整个while循环
                break;
            } else if ( recoverFromDecodeError(ret) ) {
                continue;
            } else {
                // 发生了一个真正的解码错误
                break;
            }
        }
        // 错误与 EOF 都可能返回短读，接口不单独区分二者；已交付前缀仍有效。
        return frames_decoded_total;
    }

    /// @brief 借用目标 PCM 格式，名称不表示媒体原始压缩采样格式。
    inline const AudioDataFormat& iceformat() const { return ice_format; }

    /// @brief 返回初始化时的时长估计，未知为零，不累计实际 read 返回值。
    inline size_t frames() const { return total_frames; }

private:
    /// @brief 估算当前 packet 在目标采样率下占用的帧数。
    /// @param packet 由 FFmpeg 读取到的压缩音频包。
    /// @return packet 时长对应的目标帧数；未知时返回 0。
    size_t estimatePacketTargetFrames(const AVPacket* packet) const
    {
        if ( !packet || packet->duration <= 0 || stream_index < 0 ||
             ice_format.samplerate == 0 ) {
            // 未知或无效时间信息不能转成无符号长度，避免制造巨量静音分配。
            return 0;
        }

        const AVRational targetTimeBase{
            1, static_cast<int>(ice_format.samplerate)
        };
        // 包时长属于流时间基，坏包静音却必须以目标 PCM 帧数补偿。
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

        // 只补已知时长，重置解码器以脱离损坏状态；不回滚已经交付的前缀。
        queueRecoveredSilence(m_pendingPacketTargetFrames);
        m_pendingPacketTargetFrames = 0;
        av_frame_unref(avframe);
        // 损坏帧引用不带入下一次 receive，已生成的补偿 PCM 独立保存在转换缓冲。
        avcodec_flush_buffers(avcodec_ctx);
        // 这里不重启 swr，错误前的重采样延迟仍可能在后续输出中出现。
        return true;
    }

    /// @brief 将损坏 packet 的时长补成静音，保持时间轴尽量不漂移。
    /// @param frame_count 需要补偿的目标帧数。
    /// @pre 旧 conversion_buffer 余量已耗尽，本次写入将替换整段待交付内容。
    void queueRecoveredSilence(size_t frame_count)
    {
        if ( frame_count == 0 || ice_format.channels == 0 ) {
            // 无可靠包时长时不猜测缺口长度，继续读取而非凭空拉长时间轴。
            return;
        }

        if ( frame_count > conversion_buffer.num_frames() ) {
            // 较长损坏包也需完整补偿，扩容发生在解码侧而非实时播放侧。
            conversion_buffer.resize(ice_format, frame_count);
        }

        for ( uint16_t ch = 0; ch < ice_format.channels; ++ch ) {
            std::fill_n(conversion_buffer.raw_ptrs()[ch], frame_count, 0.0F);
        }

        conversion_buffer_offset  = 0;
        conversion_buffer_remains = frame_count;
        // 与普通重采样结果共用余量协议，使静音也能按任意 chunksize 分次读出。
    }

    /// @brief 确保重采样输出缓冲能容纳当前输入可能产生的所有帧。
    /// @param input_sample_count 即将送入 swr_convert 的输入采样帧数。
    /// @return 可传给 swr_convert 的输出容量。
    /// @warning
    /// 每个解码帧及排尾调用，容量不足时允许分配，不能用于无分配热路径。
    int ensure_conversion_buffer_capacity(int input_sample_count)
    {
        int required =
            swr_ctx ? swr_get_out_samples(swr_ctx, input_sample_count) : 0;
        if ( required <= 0 ) {
            // 容量估计失败时沿用现有正容量，让 swr_convert 的结果决定后续输出。
            required = static_cast<int>(conversion_buffer.num_frames());
        }
        if ( required <= 0 ) {
            required = 1;
        }
        // 保留已有更大容量，避免不同包长导致反复收缩与扩容。

        const auto required_frames = static_cast<size_t>(required);
        if ( required_frames > conversion_buffer.num_frames() ) {
            conversion_buffer.resize(ice_format, required_frames);
        }
        return static_cast<int>(conversion_buffer.num_frames());
    }

    /// @brief 将单声道转换结果复制到所有目标声道，保证 mono
    /// 文件以居中立体声输出。
    /// @param frame_count 本次实际转换帧数，只覆盖有效前缀而不改写整个容量。
    void duplicateMonoChannelToTargetChannels(size_t frame_count)
    {
        if ( !m_duplicateMonoToTargetChannels || frame_count == 0 ||
             ice_format.channels < 2 ) {
            // 非单声道源保留 swr 的布局混音结果，不覆写其独立通道内容。
            return;
        }

        float**      channel_ptrs = conversion_buffer.raw_ptrs();
        const float* mono_src     = channel_ptrs[0];
        // 使用首输出通道而非原始压缩帧，因此复制后的各通道共享相同重采样时序。
        for ( uint16_t ch = 1; ch < ice_format.channels; ++ch ) {
            // 从第二个目标声道开始，首通道既是源也是最终输出，不能先改写它。
            std::memcpy(
                channel_ptrs[ch], mono_src, frame_count * sizeof(float));
        }
    }

    /// @brief 拥有媒体输入，音频流及 codecpar 均借用其内部存储。
    AVFormatContext* avfmt_ctx{ nullptr };
    /// @brief 独占解码状态，seek 与损坏包恢复会清除延迟帧。
    AVCodecContext* avcodec_ctx{ nullptr };
    /// @brief 借用库的静态解码器描述，不由本实例释放。
    const AVCodec* avcodec{ nullptr };
    /// @brief 复用压缩包容器，每次读取前解除旧数据引用。
    AVPacket* avpacket{ nullptr };
    /// @brief 复用解码帧容器，转换结束后解除 PCM 引用。
    AVFrame* avframe{ nullptr };
    /// @brief 独占重采样及格式转换状态，含跨帧滤波延迟。
    SwrContext* swr_ctx{ nullptr };
    /// @brief 目标输出格式，初始化后保持不变。
    AudioDataFormat ice_format;
    /// @brief 平面目标 PCM 暂存，容量可大于本次转换产生的有效帧数。
    AudioBuffer conversion_buffer;
    /// @brief 标记当前源文件是否需要在解码后复制单声道到目标多声道。
    bool m_duplicateMonoToTargetChannels{ false };
    /// @brief 最近送入解码器的 packet 在目标采样率下的估算帧数。
    size_t m_pendingPacketTargetFrames{ 0 };
    /// @brief 尚未交付的有效帧数，为零后才允许覆盖转换缓冲。
    size_t conversion_buffer_remains = 0;
    /// @brief 下一段拷贝的帧偏移，与 remains 共同描述有效余量。
    size_t conversion_buffer_offset = 0;
    /// @brief 源音频采样率，供重采样和异常元信息兜底使用。
    int m_sourceSampleRate{ 0 };

    /// @brief 流级与容器级时长换算后的较大估计，未知时为零。
    size_t total_frames{ 0 };
    /// @brief 选中的首个音频流索引，未找到时保持负值并令构造失败。
    int stream_index{ -1 };
};

/// @brief 在公开包装内部建立独占后端，不对外暴露 FFmpeg 资源类型。
/// @warning 失败可能传播既有 load_error，调用方必须在非实时加载流程处理。
FFmpegDecoderInstance::FFmpegDecoderInstance(
    std::string_view file_path, const ice::AudioDataFormat& target_format)
{
    // 初始化解码器资源
    ffimpl = std::make_unique<FFmpegDecoder>(file_path, target_format);
}
/// @brief 在后端完整定义可见处释放唯一所有权，确保资源析构可实例化。
FFmpegDecoderInstance::~FFmpegDecoderInstance() = default;

/// @brief 将目标帧寻道交给后端，保留其非精确定位和失败语义。
bool FFmpegDecoderInstance::seek(size_t pos)
{
    // 直接传播失败，不把 seek 后可能部分重置的状态包装成成功。
    return ffimpl->seek_to_frame(pos);
}

/// @brief 将至多 chunksize 帧写入调用方平面，返回有效帧数而不填充剩余空间。
/// @warning 按需解码入口包含文件 IO 与扩容，应由加载或预读线程调用。
size_t FFmpegDecoderInstance::read(float** buffer, size_t chunksize)
{
    // 包装层不维护额外游标，定位与余量协议全部由同一后端实例管理。
    return ffimpl->decode(buffer, chunksize);
}

/// @brief 借用后端固定的 PCM 目标格式，引用不跨越实例析构。
const AudioDataFormat& FFmpegDecoderInstance::get_source_format() const
{
    return ffimpl->iceformat();
}
/// @brief 返回媒体时长估计，不能用于证明已经交付的 PCM 长度。
size_t FFmpegDecoderInstance::get_source_total_frames() const
{
    return ffimpl->frames();
}

}  // namespace ice
