#include <ice/manage/dec/ffmpeg/FFmpegDecoderInstance.hpp>

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

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <string_view>

namespace ice
{
namespace
{

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
    // 音频时间基必须为正有理数，缺失或异常字段不能参与整数时间换算。
    if ( duration <= 0 || sampleRate <= 0 || timeBase.num <= 0 ||
         timeBase.den <= 0 ) {
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

// 初始化和运行期失败均以返回值传播，宏只求值一次以保留原错误码。
#define AVCALL_CHECKRETB(func)         \
    if ( !checkSetupResult((func)) ) { \
        return false;                  \
    }

}  // namespace

/// @brief 独占一个媒体输入、解码器和重采样器，并缓存尚未交付的目标 PCM。
/// 所有方法由调用方串行使用，内部没有保护 seek/read 的锁。
/// @warning 文件解码可能分配或阻塞，不属于音频设备实时回调。
class FFmpegDecoder
{
public:
    /// @brief 选择首个音频流并准备平面浮点目标格式。
    /// @param file_path 媒体路径视图，构造期间复制为零结尾字符串。
    /// @param target_format 输出格式，采样率与声道数须有效且保持稳定。
    /// @warning 初始化失败保留无效状态，已取得句柄由析构统一释放。
    explicit FFmpegDecoder(std::string_view            file_path,
                           const ice::AudioDataFormat& target_format)
    {
        // 对象完成构造后仍可查询失败状态，工厂据此丢弃实例并执行统一清理。
        m_ready = initialize(file_path, target_format);
    }

    /// @brief 查询解码资源是否可用，初始化或重启失败的实例不得参与解码。
    bool isValid() const { return m_ready; }

    /// @brief 查询最近一次已记录的准备或定位错误，不分配诊断字符串。
    int getLastSetupErrorCode() const { return m_lastSetupErrorCode; }

private:
    /// @brief 将准备及定位检查点的 FFmpeg 失败保存为可查询的错误码。
    /// @param result 被检查调用的原始返回值，非负值均视为成功。
    /// @return 失败时返回 false；成功不清除此前的诊断。
    /// 只记录现有准备及定位检查点，不代表所有失败分支都提供了错误码。
    bool checkSetupResult(int result)
    {
        // 错误路径只复制整数，诊断文本的生成与输出由非实时调用方决定。
        if ( result < 0 ) {
            m_lastSetupErrorCode = result;
            return false;
        }
        return true;
    }

    /// @brief 按依赖顺序准备资源，任一 C 接口失败立即返回。
    /// @pre 仅在构造期间调用一次，所有句柄初始为空。
    /// @return 全部资源可用时为 true，部分资源仍由实例拥有。
    bool initialize(std::string_view            file_path,
                    const ice::AudioDataFormat& target_format)
    {
        // 直接构造也必须验证完整路径，不能仅依赖上层音频池的检查。
        if ( file_path.empty() ||
             file_path.find('\0') != std::string_view::npos )
            return false;
        ice_format = target_format;
        // FFmpeg 的采样率及时间基分母使用 int，不能让无符号高位变成负值。
        if ( target_format.channels == 0 || target_format.samplerate == 0 ||
             target_format.samplerate >
                 static_cast<uint32_t>(std::numeric_limits<int>::max()) )
            return false;
        const std::string path_str(file_path);
        // C 接口需要零结尾，局部字符串至少存活到输入打开完成。
        AVCALL_CHECKRETB(
            avformat_open_input(&avfmt_ctx, path_str.c_str(), nullptr, nullptr))

        // 探测补全流参数与时长，不能只依赖容器初始头部信息。
        AVCALL_CHECKRETB(avformat_find_stream_info(avfmt_ctx, nullptr))

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
            m_lastSetupErrorCode = AVERROR_STREAM_NOT_FOUND;
            ice_format.channels  = 0;
            return false;
        }

        // 获取音频流的参数
        auto& audio_stream = avfmt_ctx->streams[stream_index];
        auto& codec_params = audio_stream->codecpar;
        // 后续寻道和坏包时长估计依赖同一时间基；拒绝无效值而不是猜测单位。
        if ( audio_stream->time_base.num <= 0 ||
             audio_stream->time_base.den <= 0 )
            return false;

        ice_format = target_format;

        // 获取总采样数。部分容器的 stream duration 会短到只有首个 packet
        // 附近，需与容器级 duration 比较后取更可信的较大值。
        const std::int64_t bestFrameCount = probeBestFrameCount(
            avfmt_ctx, audio_stream, static_cast<int>(ice_format.samplerate));
        if ( bestFrameCount > 0 ) {
            total_frames = static_cast<size_t>(bestFrameCount);
        } else {
            // 未知时长继续由零帧数表达，不在库内部向终端输出文件路径。
            // 零是未知长度哨兵，不表示 read 一定没有可读音频。
            total_frames = 0;
        }

        // 查找编解码器
        avcodec = avcodec_find_decoder(codec_params->codec_id);
        if ( !avcodec ) {
            return false;
        }

        // 分配编解码器
        avcodec_ctx = avcodec_alloc_context3(avcodec);
        if ( !avcodec_ctx ) {
            return false;
        }

        // 将流的参数拷贝到解码器上下文中
        // 它告诉解码器要处理的数据的采样率,声道,格式等信息
        AVCALL_CHECKRETB(
            avcodec_parameters_to_context(avcodec_ctx, codec_params))

        avcodec_ctx->err_recognition |= AV_EF_IGNORE_ERR;
        // 请求尽量产出可恢复的帧；真正返回 INVALIDDATA 时另行补静音。
        avcodec_ctx->flags |= AV_CODEC_FLAG_OUTPUT_CORRUPT;

        // 打开解码器
        AVCALL_CHECKRETB(avcodec_open2(avcodec_ctx, avcodec, nullptr))

        m_sourceSampleRate =
            resolve_source_sample_rate(avcodec_ctx, codec_params, ice_format);
        // 包与帧分配失败同样由返回值传播，后续不允许向库传入空容器。
        avpacket = av_packet_alloc();
        // 包申请失败时立即停止准备，避免内存紧张时再申请无法使用的帧容器。
        if ( !avpacket ) return false;
        avframe = av_frame_alloc();
        // 已取得的包仍归实例所有，帧申请失败由统一析构路径回收它。
        if ( !avframe ) return false;

        AVChannelLayout sourceLayout{};
        const int       layoutResult = make_source_channel_layout(
            &sourceLayout, avcodec_ctx, codec_params, ice_format);
        if ( layoutResult < 0 ) {
            // 自定义布局可能含动态映射，失败路径也必须释放临时存储。
            av_channel_layout_uninit(&sourceLayout);
            return false;
        }
        m_duplicateMonoToTargetChannels =
            sourceLayout.nb_channels == 1 && ice_format.channels > 1;
        // 单声道经重采样后复制首通道，使全部目标声道保持一致的居中内容。

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
        // 先释放临时布局再传播配置错误，避免提前返回遗留布局存储。
        AVCALL_CHECKRETB(swrAllocRet)

        if ( !swr_ctx ) {
            return false;
        }

        // 初始化重采样器上下文
        AVCALL_CHECKRETB(swr_init(swr_ctx))
        // 分配和初始化是两个阶段；非空上下文不代表重采样配置已可执行。

        // 初始容量只是常见帧长，实际解码帧与重采样延迟可能触发后续扩容。
        const size_t max_frames_per_avframe = 2048;
        if ( !conversion_buffer.resize(ice_format, max_frames_per_avframe) )
            return false;
        return true;
    }

public:
    /// @brief 释放初始化成功或部分失败实例持有的媒体资源，不补齐剩余 PCM。
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
        // 直接构造的失败实例与工厂空返回保持一致，不访问未初始化句柄。
        if ( !m_ready ) return false;
        // 时间换算接受有符号 64 位帧数，超范围请求不能截断成负时间戳。
        if ( frame_offset >
             static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) )
            return false;
        // 从头重放要求恢复完整初始状态；仅 flush 不保证重置 AAC 等解码器的
        // 噪声合成历史。先准备候选上下文，失败时不破坏仍可用的旧实例。
        // 释放函数只管理候选上下文，不捕获当前实例；失败返回也能完整回收。
        // 容器和重采样格式保持不变，重新打开解码器仍使用原流参数。
        // 这里的分配仅在后台定位执行，设备回调只访问已发布的 PCM 页。
        /// @brief 释放尚未交给实例接管的候选解码上下文。
        const auto releaseContext = [](AVCodecContext* context) {
            avcodec_free_context(&context);
        };
        std::unique_ptr<AVCodecContext, decltype(releaseContext)> freshContext(
            nullptr, releaseContext);
        if ( frame_offset == 0 ) {
            freshContext.reset(avcodec_alloc_context3(avcodec));
            if ( !freshContext ||
                 !checkSetupResult(avcodec_parameters_to_context(
                     freshContext.get(),
                     avfmt_ctx->streams[stream_index]->codecpar)) ) {
                return false;
            }
            // 保留原有损坏包容错策略，避免重放后使用不同的错误处理配置。
            // 后端专有初始状态由 avcodec_open2 重建，不能复制已运行的内部状态。
            freshContext->err_recognition = avcodec_ctx->err_recognition;
            freshContext->flags           = avcodec_ctx->flags;
            // 候选失败只记录诊断，不清空仍属于旧解码位置的 PCM 余量。
            if ( !checkSetupResult(
                     avcodec_open2(freshContext.get(), avcodec, nullptr)) ) {
                return false;
            }
        }
        // 将帧偏移量转换回FFmpeg的时间基单位
        int64_t timestamp =
            av_rescale_q(frame_offset,
                         { 1, static_cast<int>(ice_format.samplerate) },
                         avfmt_ctx->streams[stream_index]->time_base);
        // 输入帧非负，合法换算结果也必须非负；溢出哨兵不能当作媒体时间使用。
        // 此时尚未改变解码位置或缓存，拒绝请求后原实例仍可继续使用。
        if ( timestamp < 0 ) return false;

        // 执行寻道操作
        // AVSEEK_FLAG_BACKWARD 保证我们能seek到请求的关键帧或其之前最近的关键帧
        AVCALL_CHECKRETB(av_seek_frame(
            avfmt_ctx, stream_index, timestamp, AVSEEK_FLAG_BACKWARD))

        // 成功寻道后丢弃旧位置的延迟帧、包引用及目标 PCM，避免新旧位置串音。
        avcodec_flush_buffers(avcodec_ctx);
        av_packet_unref(avpacket);
        av_frame_unref(avframe);
        if ( freshContext ) {
            // 已解除旧帧引用后再替换上下文，候选由现有析构路径接管。
            avcodec_free_context(&avcodec_ctx);
            avcodec_ctx = freshContext.release();
        }
        conversion_buffer_remains   = 0;
        conversion_buffer_offset    = 0;
        m_pendingPacketTargetFrames = 0;
        m_recoveredSilenceFrames    = 0;
        if ( swr_ctx ) {
            // 输入位置已改变，关闭重采样器后不再有可继续读取的旧状态。
            // 先标记失效，重启失败保持失效，要求调用方重新创建实例。
            m_ready = false;
            swr_close(swr_ctx);
            AVCALL_CHECKRETB(swr_init(swr_ctx))
            m_ready = true;
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
        // 初始化或寻道重启失败时不触碰输出，也不隐式重试不可用的资源。
        if ( !m_ready || !buffer || chunksize == 0 ) return 0;
        // 验证先于读取压缩包和消耗余量，空声道不会推进位置或部分改写输出。
        for ( uint16_t channel = 0; channel < ice_format.channels; ++channel )
            if ( !buffer[channel] ) return 0;
        size_t frames_decoded_total = 0;

        while ( frames_decoded_total < chunksize ) {
            // 静音只记录剩余时长，按调用方本次请求直接写入，不按包时长分配。
            // 巨大损坏包也只消耗当前输出块的工作量，后续 read 继续交付余量。
            if ( m_recoveredSilenceFrames > 0 ) {
                const size_t frames = std::min(chunksize - frames_decoded_total,
                                               m_recoveredSilenceFrames);
                for ( uint16_t ch = 0; ch < ice_format.channels; ++ch )
                    std::fill_n(
                        buffer[ch] + frames_decoded_total, frames, 0.0F);
                m_recoveredSilenceFrames -= frames;
                frames_decoded_total += frames;
                continue;
            }
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
                if ( output_capacity <= 0 ) {
                    av_frame_unref(avframe);
                    break;
                }
                // 平面音频可能超过 data 的固定槽数，扩展指针表覆盖全部声道。
                // 普通交错格式也通过同一字段提供输入，不依赖固定数组容量。
                int converted_count =
                    swr_convert(swr_ctx,
                                (uint8_t**)conversion_buffer.raw_ptrs(),
                                output_capacity,
                                (const uint8_t**)avframe->extended_data,
                                avframe->nb_samples);

                // 负值是真正失败，不能跳过此帧后把后续 PCM 拼到同一请求中。
                if ( converted_count < 0 ) {
                    av_frame_unref(avframe);
                    // 转换失败不属于坏包补静音策略，释放帧并返回此前已交付的前缀。
                    m_pendingPacketTargetFrames = 0;
                    break;
                }
                // 零输出可能只是滤波器积累输入，仍须继续解码以取得后续输出。
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
                // 将排尾结果纳入普通余量协议，避免小块读取丢弃未交付样本。
                // 每轮最多转换一次；请求已满时由循环条件退出，余量留给下次
                // read。
                const int output_capacity =
                    ensure_conversion_buffer_capacity(0);
                if ( output_capacity <= 0 ) break;
                const int flushed_count =
                    swr_convert(swr_ctx,
                                (uint8_t**)conversion_buffer.raw_ptrs(),
                                output_capacity,
                                nullptr,
                                0);
                if ( flushed_count <= 0 ) break;
                duplicateMonoChannelToTargetChannels(
                    static_cast<size_t>(flushed_count));
                conversion_buffer_offset  = 0;
                conversion_buffer_remains = static_cast<size_t>(flushed_count);
                continue;
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
    inline size_t frames() const { return m_ready ? total_frames : 0; }

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
        // 损坏帧引用不带入下一次 receive，补偿余量由独立计数保存。
        avcodec_flush_buffers(avcodec_ctx);
        // 这里不重启 swr，错误前的重采样延迟仍可能在后续输出中出现。
        return true;
    }

    /// @brief 将损坏 packet 的时长补成静音，保持时间轴尽量不漂移。
    /// @param frame_count 需要补偿的目标帧数。
    /// @pre 旧 PCM 和静音余量已耗尽，调用后先交付本次补偿再读取新包。
    void queueRecoveredSilence(size_t frame_count)
    {
        // 无可靠包时长时不猜测缺口，不因元信息申请整段静音存储。
        m_recoveredSilenceFrames = ice_format.channels == 0 ? 0 : frame_count;
    }

    /// @brief 确保重采样输出缓冲能容纳当前输入可能产生的所有帧。
    /// @param input_sample_count 即将送入 swr_convert 的输入采样帧数。
    /// @return 可传给 swr_convert 的正容量，输入或估计失败时为零。
    /// @warning
    /// 每个解码帧及排尾调用，容量不足时允许分配，不能用于无分配热路径。
    int ensure_conversion_buffer_capacity(int input_sample_count)
    {
        // 无有效上下文或负输入帧数时不能向重采样器提交请求。
        if ( !swr_ctx || input_sample_count < 0 ) return 0;
        const int estimate = swr_get_out_samples(swr_ctx, input_sample_count);
        // 负值是 API 错误，不是零输出；沿用旧容量会掩盖重采样状态失败。
        if ( estimate < 0 ) return 0;
        // 零上界仍保留至少一个输出槽，正常排尾由 swr_convert 返回零结束。
        const int required = std::max(estimate, 1);
        // 保留已有更大容量，避免不同包长导致反复收缩与扩容。

        const auto required_frames = static_cast<size_t>(required);
        if ( required_frames > conversion_buffer.num_frames() ) {
            // 返回零容量使读取结束，不把旧容量伪装成本次扩容成功。
            if ( !conversion_buffer.resize(ice_format, required_frames) )
                return 0;
        }
        // 内部容量使用 size_t，只向外部 API 暴露 int 可表达的有效前缀。
        // 先在 size_t 域内截断，禁止窄化后才检查符号或上下界。
        return static_cast<int>(
            std::min(conversion_buffer.num_frames(),
                     static_cast<size_t>(std::numeric_limits<int>::max())));
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

    /// @brief 初始化成功后置位，寻道重启失败清零，阻止访问不可用的资源。
    bool m_ready{ false };
    /// @brief 最近一次准备或定位诊断点的负错误码；零仅表示尚未记录诊断。
    /// 与 seek/read 一样由调用方串行访问，不用于跨线程发布状态。
    int m_lastSetupErrorCode{ 0 };

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
    /// @brief 待交付静音帧数，仅为逻辑时长，不拥有 PCM；寻道成功后清零。
    size_t m_recoveredSilenceFrames{ 0 };
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
/// @warning 仅在非实时加载流程构造；C 接口失败可通过 isValid 查询。
FFmpegDecoderInstance::FFmpegDecoderInstance(
    std::string_view file_path, const ice::AudioDataFormat& target_format)
{
    // 初始化解码器资源
    ffimpl = std::make_unique<FFmpegDecoder>(file_path, target_format);
}
/// @brief 在后端完整定义可见处释放唯一所有权，确保资源析构可实例化。
FFmpegDecoderInstance::~FFmpegDecoderInstance() = default;

/// @brief 将后端初始化结果暴露给工厂，不以零时长替代成功状态。
bool FFmpegDecoderInstance::isValid() const
{
    return ffimpl->isValid();
}

/// @brief 返回后端保留的准备或定位诊断，不触发格式化、日志或状态清理。
int FFmpegDecoderInstance::getLastSetupErrorCode() const
{
    return ffimpl->getLastSetupErrorCode();
}

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
