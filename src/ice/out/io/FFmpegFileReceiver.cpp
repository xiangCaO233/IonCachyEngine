#include <ice/out/io/FFmpegFileReceiver.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/mathematics.h>
#include <libavutil/mem.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

namespace ice
{
namespace
{

/// @brief 输出容器和编码器选择结果。
// 名称借用静态字符串，不负责释放；容器与编码器可以分别覆盖。
struct OutputFormatSelection {
    /// @brief 显式指定的 FFmpeg muxer 名称；nullptr 表示按路径自动推断。
    const char* formatName{ nullptr };

    /// @brief 显式指定的编码器；AV_CODEC_ID_NONE 表示使用 muxer 默认值。
    AVCodecID codecOverride{ AV_CODEC_ID_NONE };

    /// @brief 优先尝试的编码器名称；不可用时再按 codec ID 查找。
    const char* preferredCodecName{ nullptr };
};

/// @brief 将文件系统路径转换为 FFmpeg 使用的 UTF-8 字符串。
/// @param path 文件系统路径。
/// @return UTF-8 路径字符串。
std::string path_to_utf8(const std::filesystem::path& path)
{
    // char8_t 与 char 的指针类型不同，这里复制 UTF-8
    // 字节，不使用本地代码页转码。 返回独立字符串，使 FFmpeg 调用期间不依赖临时
    // u8string 的存储。
    // 路径转换及字节复制会分配存储，仅用于离线文件入口。
    auto u8Path = path.u8string();
    return std::string(reinterpret_cast<const char*>(u8Path.c_str()),
                       u8Path.size());
}

/// @brief 转为小写扩展名。
/// @param path 输出路径。
/// @return 小写扩展名。
std::string lowercase_extension(const std::filesystem::path& path)
{
    // 只规范扩展名，不改变文件路径主体，避免破坏大小写敏感文件系统上的名称。
    std::string extension = path_to_utf8(path.extension());
    std::transform(
        extension.begin(),
        // 先转换 unsigned char，避免高位字节作为负 char 传入 tolower。
        extension.end(),
        extension.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return extension;
}

/// @brief 根据输出路径选择容器和编码器。
/// @param outputPath 输出路径。
/// @return 输出格式选择结果。
OutputFormatSelection select_output_format(
    const std::filesystem::path& outputPath)
{
    // 选择过程只看扩展名，不探测已有文件内容，也不读取文件确认格式。
    const std::string extension = lowercase_extension(outputPath);
    if ( extension == ".m4a" ) {
        // M4A 明确采用 MP4 容器和 AAC，不能仅把扩展名当作 codec 名称。
        return OutputFormatSelection{ "mp4", AV_CODEC_ID_AAC };
    }
    if ( extension == ".opus" ) {
        // Opus 编码放入 OGG 封装，文件名本身不要求使用同名 muxer。
        return OutputFormatSelection{ "ogg", AV_CODEC_ID_OPUS };
    }
    if ( extension == ".mp3" ) {
        // 优先使用外部 LAME 实现，同时保留 MP3 ID 作为缺少命名编码器时的后备。
        return OutputFormatSelection{ nullptr, AV_CODEC_ID_MP3, "libmp3lame" };
    }
    // 其余扩展名交回 muxer 推断，不在此提前宣称容器或编码器一定可用。
    return OutputFormatSelection{};
}

/// @brief 按优选名称和编码 ID 查找输出编码器。
/// @param outputSelection 容器选择中的优选编码器设置。
/// @param codecId 显式覆盖或 muxer 默认指定的编码 ID。
/// @return 可用编码器的非拥有指针，均未找到时返回 nullptr。
const AVCodec* find_output_encoder(const OutputFormatSelection& outputSelection,
                                   AVCodecID                    codecId)
{
    if ( outputSelection.preferredCodecName ) {
        // 名称优先只影响首次选择，没有匹配时仍允许使用当前构建中的其他同类实现。
        const AVCodec* preferred =
            avcodec_find_encoder_by_name(outputSelection.preferredCodecName);
        if ( preferred ) {
            return preferred;
        }
    }
    // 找到描述符不代表后续格式协商或编码器初始化会成功。
    return avcodec_find_encoder(codecId);
}

/// @brief 将 FFmpeg 错误码转换为文本。
/// @param code FFmpeg 错误码。
/// @return 错误文本。
std::string ffmpeg_error_string(int code)
{
    // 缓冲先清零；错误文本生成未成功时也不读取未初始化字节。
    char buffer[AV_ERROR_MAX_STRING_SIZE] = { 0 };
    av_strerror(code, buffer, sizeof(buffer));
    return buffer;
}

/// @brief 判断错误码是否表示编码器暂时无包可取。
/// @param code FFmpeg 错误码。
/// @return 需要停止 drain 但不是失败时返回 true。
bool is_packet_drain_finished(int code)
{
    // EAGAIN 是本轮暂无输出，EOF 是编码器已排空；两者只共用停止取包的判断。
    // 此函数不能用于把 send_frame 的所有负返回值也当作可忽略状态。
    return code == AVERROR(EAGAIN) || code == AVERROR_EOF;
}

/// @brief 判断编码器是否支持指定采样格式。
/// @param codec 编码器。
/// @param sampleFormat 采样格式。
/// @return 支持时返回 true。
bool supports_sample_format(const AVCodec* codec, AVSampleFormat sampleFormat)
{
    const void* configs     = nullptr;
    int         configCount = 0;
    const int   configRet =
        avcodec_get_supported_config(nullptr,
                                     codec,
                                     AV_CODEC_CONFIG_SAMPLE_FORMAT,
                                     0,
                                     &configs,
                                     &configCount);
    if ( configRet < 0 || !configs ) {
        // 无法取得能力表时按可尝试处理，把最终拒绝交给 avcodec_open2。
        // 这不是已验证支持格式的证据，也没有将查询失败单独暴露给调用者。
        return true;
    }

    const auto* formats = static_cast<const AVSampleFormat*>(configs);
    // 能力表为非拥有只读数据，不释放；同时尊重计数与结束标识防止越界扫描。
    for ( int index = 0;
          index < configCount && formats[index] != AV_SAMPLE_FMT_NONE;
          ++index ) {
        if ( formats[index] == sampleFormat ) {
            return true;
        }
    }
    return false;
}

/// @brief 选择编码器支持的采样格式。
/// @param codec 编码器。
/// @return 采样格式。
AVSampleFormat select_sample_format(const AVCodec* codec)
{
    // 优先浮点以贴近引擎输入，随后才回退整数或双精度，并不等于最终文件位深。
    constexpr AVSampleFormat preferredFormats[] = {
        AV_SAMPLE_FMT_FLTP, AV_SAMPLE_FMT_FLT,  AV_SAMPLE_FMT_S16P,
        AV_SAMPLE_FMT_S16,  AV_SAMPLE_FMT_S32P, AV_SAMPLE_FMT_S32,
        AV_SAMPLE_FMT_DBLP, AV_SAMPLE_FMT_DBL,  AV_SAMPLE_FMT_NONE,
    };

    for ( const AVSampleFormat format : preferredFormats ) {
        // NONE 只是本地候选列表终止符，不能传给编码器作为真实采样格式。
        if ( format == AV_SAMPLE_FMT_NONE ) {
            break;
        }
        if ( supports_sample_format(codec, format) ) {
            return format;
        }
    }

    const void* configs     = nullptr;
    int         configCount = 0;
    const int   configRet =
        avcodec_get_supported_config(nullptr,
                                     codec,
                                     AV_CODEC_CONFIG_SAMPLE_FORMAT,
                                     0,
                                     &configs,
                                     &configCount);
    if ( configRet >= 0 && configs && configCount > 0 ) {
        // 常见格式均未命中时保留编码器首选的其他格式，不强制转换成某个固定整数位深。
        return static_cast<const AVSampleFormat*>(configs)[0];
    }
    // 无有效能力信息的最终后备仍需 avcodec_open2 验证，不能在此保证可编码。
    return AV_SAMPLE_FMT_FLTP;
}

/// @brief 选择编码器支持的采样率。
/// @param codec 编码器。
/// @param desiredSampleRate 期望采样率。
/// @return 采样率。
int select_sample_rate(const AVCodec* codec, std::uint32_t desiredSampleRate)
{
    // 调用链需提供可表示为 int 的采样率；这里没有对 uint32_t 的上界另行验证。
    // 输出采样率只描述编码器输入时钟，转换后帧数不与引擎输入帧计数直接相等。
    const auto  desired     = static_cast<int>(desiredSampleRate);
    const void* configs     = nullptr;
    int         configCount = 0;
    const int   configRet   = avcodec_get_supported_config(
        nullptr, codec, AV_CODEC_CONFIG_SAMPLE_RATE, 0, &configs, &configCount);
    if ( configRet < 0 || !configs || configCount <= 0 ) {
        // 无能力表时尝试原请求值，不自动改用 44.1kHz 或 48kHz。
        return desired;
    }

    const auto* sampleRates = static_cast<const int*>(configs);
    int         fallback    = 0;
    // 只寻找精确匹配；后备使用表中首项，不是选择数值最接近的采样率。
    for ( int index = 0; index < configCount && sampleRates[index] != 0;
          ++index ) {
        if ( fallback == 0 ) {
            fallback = sampleRates[index];
        }
        if ( sampleRates[index] == desired ) {
            return desired;
        }
    }
    return fallback > 0 ? fallback : desired;
}

/// @brief 选择编码器支持的声道布局。
/// @param codec 编码器。
/// @param desiredChannels 期望声道数。
/// @param output 输出声道布局。
/// @return FFmpeg 错误码；成功为非负。
int select_channel_layout(const AVCodec* codec, int desiredChannels,
                          AVChannelLayout* output)
{
    if ( !output || desiredChannels <= 0 ) {
        // 无目标对象或非正声道数不能建立布局；输出指针必须由调用方提供。
        return AVERROR(EINVAL);
    }

    // 输出布局应为可接收副本的对象，本函数不替调用方回收此前的布局。
    AVChannelLayout desiredLayout{};
    // 仅按声道数生成默认排列，没有从上游传入自定义声道顺序。
    av_channel_layout_default(&desiredLayout, desiredChannels);

    const void* configs     = nullptr;
    int         configCount = 0;
    const int   configRet =
        avcodec_get_supported_config(nullptr,
                                     codec,
                                     AV_CODEC_CONFIG_CHANNEL_LAYOUT,
                                     0,
                                     &configs,
                                     &configCount);
    if ( configRet >= 0 && configs && configCount > 0 ) {
        const auto* layouts = static_cast<const AVChannelLayout*>(configs);
        const AVChannelLayout* fallback = nullptr;
        // 表中的布局只借用，选中后复制到输出对象，避免输出依赖静态能力表的存储。
        for ( int index = 0;
              index < configCount && layouts[index].nb_channels > 0;
              ++index ) {
            const AVChannelLayout* layout = &layouts[index];
            if ( !fallback ) {
                fallback = layout;
            }
            if ( layout->nb_channels == desiredChannels ) {
                // 匹配的是数量而非逐声道身份，相同声道数也不保证排列完全等同。
                const int ret = av_channel_layout_copy(output, layout);
                av_channel_layout_uninit(&desiredLayout);
                return ret;
            }
        }

        if ( fallback ) {
            // 未找到相同数量时退到首个布局，后续重采样器负责实际声道转换。
            const int ret = av_channel_layout_copy(output, fallback);
            av_channel_layout_uninit(&desiredLayout);
            return ret;
        }
    }

    // 能力表不可用时尝试默认布局；临时布局在所有成功选择路径都需解除其内部资源。
    const int ret = av_channel_layout_copy(output, &desiredLayout);
    av_channel_layout_uninit(&desiredLayout);
    return ret;
}

/// @brief 判断编码格式是否需要设置默认码率。
/// @param codecId 编码格式。
/// @return 需要设置时返回 true。
bool should_set_default_bitrate(AVCodecID codecId)
{
    // 默认有损码率只覆盖列出的 codec，不把 PCM/FLAC
    // 等格式强行设成恒定码率输出。
    return codecId == AV_CODEC_ID_MP3 || codecId == AV_CODEC_ID_AAC ||
           codecId == AV_CODEC_ID_VORBIS || codecId == AV_CODEC_ID_OPUS ||
           codecId == AV_CODEC_ID_WMAV1 || codecId == AV_CODEC_ID_WMAV2;
}

/// @brief 通过 FFmpeg 释放入口管理帧对象的删除器。
struct AVFrameDeleter {
    /// @brief 释放 AVFrame。
    /// @param frame 要释放的帧。
    void operator()(AVFrame* frame) const { av_frame_free(&frame); }
};

/// @brief FFmpeg 采样数组的 RAII 包装。
// 只能管理 FFmpeg 分配的连续样本区和指针数组，不能接管任意 AudioBuffer 的平面。
struct SampleArray {
    /// @brief 采样数据。
    uint8_t** data{ nullptr };

    /// @brief 禁止复制。
    SampleArray(const SampleArray&) = delete;

    /// @brief 禁止复制赋值。
    SampleArray& operator=(const SampleArray&) = delete;

    /// @brief 默认构造。
    SampleArray() = default;

    /// @brief 析构并释放采样数据。
    ~SampleArray() { reset(); }

    /// @brief 释放采样数据。
    void reset()
    {
        if ( !data ) {
            return;
        }
        // 连续样本区由首指针持有，先释放数据再释放声道指针数组，不能逐声道重复释放。
        av_freep(&data[0]);
        // av_freep 同时清空指针，使手动 reset 后析构再次调用也安全。
        av_freep(&data);
    }
};

}  // namespace

/// @brief 保存输出配置并为离线块预分配引擎缓冲，不在构造时打开文件。
/// @param output_path 将在 open 时使用的输出路径。
/// @param format 输入图的声道数与采样率配置。
/// @warning 低频路径：缓冲初始化可能分配内存，不用于实时音频回调。
FFmpegFileReceiver::FFmpegFileReceiver(std::filesystem::path  output_path,
                                       const AudioDataFormat& format)
    : IReceiver(format), m_outputPath(std::move(output_path)), m_format(format)
{
    m_buffer.resize(m_format, m_blockFrames);
}

/// @brief 关闭输出链路并释放资源，调用前必须保证没有并发 start。
/// @warning 析构可能编码尾部并写文件，不提供跨线程 join 或有界耗时保证。
FFmpegFileReceiver::~FFmpegFileReceiver()
{
    close();
}

/// @brief 设置输入帧预算，而非编码器最终样本数或容器包数量。
/// @param frame_count 按输入采样率计数的帧数，零值会在 start 时被拒绝。
/// @warning 配置路径：普通成员无同步，不能与离线循环并发修改。
void FFmpegFileReceiver::set_target_frames(std::size_t frame_count)
{
    // 预算按输入时钟解释，不包含编码器延迟、末帧填充或容器头尾占用。
    m_targetFrames = frame_count;
}

/// @brief 更新离线拉取块大小并调整缓冲。
/// @param frame_count 非零块帧数，零值保持原设置。
/// @warning 配置路径：会重新分配缓冲，不允许与 start 并发调用。
void FFmpegFileReceiver::set_block_frames(std::size_t frame_count)
{
    if ( frame_count == 0 ) {
        // 避免 start 每轮处理零帧却无法推进累计进度。
        return;
    }
    // 非零尺寸当前没有上限检查，后续分配和 FFmpeg int 窄化还有额外约束。
    m_blockFrames = frame_count;
    m_buffer.resize(m_format, m_blockFrames);
}

/// @brief 替换在离线编码线程同步执行的进度回调。
/// @param callback 接收已送入编码链路的累计输入帧数，空回调表示不通知。
/// @warning 不允许并发替换；回调不得重入 close 或销毁仍在执行 start 的接收端。
void FFmpegFileReceiver::set_progress_callback(
    std::function<void(std::size_t)> callback)
{
    // 保存回调不调度任务，后续通知就在 start 的调用线程执行，没有异常隔离。
    m_progressCallback = std::move(callback);
}

/// @brief 查询已接受到编码链路的输入帧计数，不保证这些帧已经持久化。
/// @return 输入采样率下的累计帧数，重采样后输出数量可能不同。
/// @warning 返回普通成员，没有原子或锁保护，其他线程应通过回调转发进度。
std::size_t FFmpegFileReceiver::frames_written() const
{
    return m_framesWritten;
}

/// @brief 借用当前错误文本，后续 open 或失败操作可能修改该字符串。
/// @return 接收端内部文本的只读引用。
/// @warning 不允许与写错误或对象销毁并发；跨线程保存需由调用者同步复制。
const std::string& FFmpegFileReceiver::error_message() const
{
    return m_errorMessage;
}

/// @brief 从关闭状态建立编码链路，失败时回收部分初始化资源。
/// @return 链路已打开或本次初始化成功时返回 true。
/// @warning 低频文件路径：可能创建或截断输出文件，不在失败时恢复原文件内容。
bool FFmpegFileReceiver::open()
{
    if ( m_opened ) {
        // 已打开时直接复用，不重置计数、错误或停止请求，也不验证输出路径变化。
        return true;
    }
    m_errorMessage.clear();
    // 新一轮才清累计输入进度和输出 PTS，停止标记也在这里清除。
    m_framesWritten  = 0;
    m_nextPts        = 0;
    m_trailerWritten = false;
    // 新 open 会清除停止请求，所以启动前的 stop 不保证在下一轮继续生效。
    m_stopRequested.store(false);

    if ( !open_encoder() ) {
        // 私有初始化分阶段写入成员句柄，统一由 close
        // 回收，失败文本保留供调用者读取。
        close();
        return false;
    }
    return true;
}

/// @brief 尝试正常收尾后释放句柄，不删除失败或取消后的部分输出文件。
/// @warning 低频耗时路径：可能进行重采样排尾、编码及文件 IO；不能与 start
/// 并发。
/// @warning running=false 只是状态发布，不意味着另一个线程已停止访问成员资源。
void FFmpegFileReceiver::close()
{
    if ( m_opened && !m_trailerWritten && m_errorMessage.empty() ) {
        // 只有正常打开且无已知错误时补尾；失败或停止路径不承诺文件含完整
        // trailer。 close 没有返回值，收尾失败只能通过错误文本观察。
        static_cast<void>(finish_encoding());
    }

    if ( m_fifo ) {
        // FIFO 先释放未提交样本；失败路径不会再尝试把残留数据编码成完整文件。
        av_audio_fifo_free(m_fifo);
        m_fifo = nullptr;
    }
    // 错误清理直接释放尚未排出的转换延迟，不构造补偿或重试输出。
    if ( m_swrContext ) {
        swr_free(&m_swrContext);
    }
    // 释放包壳及可能滞留的引用数据，FFmpeg 会同时清空成员句柄。
    if ( m_packet ) {
        av_packet_free(&m_packet);
    }
    // 内部编码延迟也随上下文释放，失败后没有恢复为可继续提交输入的逻辑。
    if ( m_codecContext ) {
        avcodec_free_context(&m_codecContext);
    }
    if ( m_formatContext ) {
        // NOFILE 容器不拥有这里打开的 AVIO 文件句柄，不能无条件关闭 pb。
        if ( !(m_formatContext->oformat->flags & AVFMT_NOFILE) &&
             m_formatContext->pb ) {
            avio_closep(&m_formatContext->pb);
        }
        avformat_free_context(m_formatContext);
    }

    // stream 随容器上下文释放，只清观察指针，不单独释放流对象。
    m_formatContext  = nullptr;
    m_codecContext   = nullptr;
    m_stream         = nullptr;
    m_nextPts        = 0;
    m_opened         = false;
    m_trailerWritten = false;
    m_running.store(false);
}

/// @brief 在调用线程拉取输入预算、编码并完成输出。
/// @details 进入编码循环后返回前关闭资源；前置检查失败则直接返回，不统一执行
/// close。
/// @return 预算处理及正常收尾均成功时返回 true；取消也返回 false。
/// @warning 离线循环：分配、编码、回调和文件写入可阻塞，禁止作为实时回调路径。
/// @warning running 的 load/store 不是互斥获取，不支持两个线程并发进入 start。
bool FFmpegFileReceiver::start()
{
    if ( m_running.load() ) {
        set_error("FFmpegFileReceiver is already running");
        return false;
    }
    if ( m_targetFrames == 0 ) {
        // 不输出零长度文件，调用者必须先给出非零输入帧预算。
        set_error("FFmpegFileReceiver target frame count is zero");
        return false;
    }
    if ( !m_opened && !open() ) {
        return false;
    }

    // 原子标志不保护整个对象，普通配置与编码资源仍需由本次调用线程独占。
    m_running.store(true);
    bool ok = true;

    while ( m_framesWritten < m_targetFrames && !m_stopRequested.load() ) {
        // 尾块只拉取剩余预算，输入计数不会因固定块长而越过目标。
        const std::size_t frameCount =
            std::min(m_blockFrames, m_targetFrames - m_framesWritten);
        // 按本块预算调整有效长度，不把上一完整块的旧尾部误作为本次有效输入。
        m_buffer.resize(m_format, frameCount);
        m_buffer.clear();
        // 未绑定音源时仍编码清零缓冲；这里没有把缺源当成错误或提前 EOF。
        if ( get_source() ) {
            get_source()->process(m_buffer);
        }
        // 失败块不计入进度，但可能已转换或写入部分数据，因此不能按计数原地重放。
        if ( !write_buffer(m_buffer, frameCount) ) {
            ok = false;
            break;
        }
        m_framesWritten += frameCount;
        // 成功入链路即可通知，不等待编码器延迟包排空或磁盘刷新。
        // 回调耗时直接延长导出任务，停止标记只能在回调返回后继续被循环观察。
        if ( m_progressCallback ) {
            m_progressCallback(m_framesWritten);
        }
    }

    if ( m_stopRequested.load() && ok ) {
        // 取消被记录为失败；已有编码错误优先保留，不被通用 stopped 文本覆盖。
        set_error("FFmpegFileReceiver stopped");
        ok = false;
    }
    if ( ok && !finish_encoding() ) {
        ok = false;
    }

    // 先清运行标记再 close；观察者不能仅凭 is_running=false
    // 就并发销毁或重配对象。
    m_running.store(false);
    close();
    return ok;
}

/// @brief 发布协作停止请求，不等待正在执行的块完成。
/// @warning 跨线程控制：循环在块边界读取；不能中断当前编码、文件 IO
/// 或进度回调。
void FFmpegFileReceiver::stop()
{
    m_stopRequested.store(true);
}

/// @brief 查询离线循环的原子状态标志，不作为资源回收完成通知。
/// @return 最近发布的运行状态。
bool FFmpegFileReceiver::is_running() const
{
    return m_running.load();
}

/// @brief 协商输出格式并依次建立容器、编码器、文件与重采样链路。
/// @return 所有阶段成功且 m_opened 已发布时返回 true。
/// @warning 离线初始化：涉及分配与文件 IO；失败由 open 调用 close
/// 回收部分句柄。
/// @warning 文件可能在后续初始化失败前已创建或截断，不提供文件内容回滚。
bool FFmpegFileReceiver::open_encoder()
{
    if ( m_format.channels == 0 || m_format.samplerate == 0 ) {
        // 零声道或零采样率无法定义转换关系，必须在创建输出文件前拒绝。
        set_error("Invalid FFmpegFileReceiver input format");
        return false;
    }

    const std::string           outputPath = path_to_utf8(m_outputPath);
    const OutputFormatSelection outputSelection =
        select_output_format(m_outputPath);
    // 显式格式覆盖优先；未覆盖时由输出路径让 FFmpeg 选择 muxer。
    int ret = avformat_alloc_output_context2(&m_formatContext,
                                             nullptr,
                                             outputSelection.formatName,
                                             outputPath.c_str());
    if ( ret < 0 || !m_formatContext || !m_formatContext->oformat ) {
        // 同时检查错误码和输出句柄，不能只凭非负返回值继续解引用上下文。
        set_ffmpeg_error("Failed to allocate output context", ret);
        return false;
    }

    // 容器与 codec 分别选择，不能把支持容器视为已具备其默认音频编码器。
    const AVCodecID codecId = outputSelection.codecOverride != AV_CODEC_ID_NONE
                                  ? outputSelection.codecOverride
                                  : m_formatContext->oformat->audio_codec;
    if ( codecId == AV_CODEC_ID_NONE ) {
        set_error("Output format does not provide a default audio encoder");
        return false;
    }

    const AVCodec* codec = find_output_encoder(outputSelection, codecId);
    if ( !codec ) {
        // 当前 FFmpeg 构建可能裁剪掉所需
        // encoder，不自动更换用户选择的输出格式。
        set_error("FFmpeg encoder is not available for output format");
        return false;
    }

    // stream 由容器上下文拥有，close 释放容器时统一回收，不单独释放该观察指针。
    m_stream = avformat_new_stream(m_formatContext, nullptr);
    if ( !m_stream ) {
        set_error("Failed to create audio stream");
        return false;
    }

    // 编码器上下文与流参数不是同一对象，先独立配置，再复制已打开后的参数到流。
    m_codecContext = avcodec_alloc_context3(codec);
    if ( !m_codecContext ) {
        set_error("Failed to allocate codec context");
        return false;
    }

    AVChannelLayout outputLayout{};
    // 输出布局允许按编码器能力回退，重采样器将处理与输入声道数不一致的情况。
    ret = select_channel_layout(
        codec, static_cast<int>(m_format.channels), &outputLayout);
    if ( ret < 0 ) {
        set_ffmpeg_error("Failed to select encoder channel layout", ret);
        return false;
    }

    m_codecContext->codec_id = codecId;
    // 接收端只创建音频流，不复制输入文件中的视频、封面或标签。
    m_codecContext->codec_type = AVMEDIA_TYPE_AUDIO;
    m_codecContext->sample_fmt = select_sample_format(codec);
    // 采样率回退后，PTS 与重采样输出都必须使用实际选中的编码采样率。
    m_codecContext->sample_rate =
        select_sample_rate(codec, m_format.samplerate);
    m_codecContext->time_base = AVRational{ 1, m_codecContext->sample_rate };
    // 显式允许实验性编码器，不把成功打开解释为所有格式均具有相同成熟度。
    m_codecContext->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;
    if ( should_set_default_bitrate(codecId) ) {
        // 有损格式统一给默认总码率，当前没有依据声道数或用户质量偏好调整。
        m_codecContext->bit_rate = 192000;
    }

    ret = av_channel_layout_copy(&m_codecContext->ch_layout, &outputLayout);
    // 上下文取得自己的布局副本，临时布局不再参与后续编码生命周期。
    av_channel_layout_uninit(&outputLayout);
    if ( ret < 0 ) {
        set_ffmpeg_error("Failed to copy encoder channel layout", ret);
        return false;
    }

    if ( m_formatContext->oformat->flags & AVFMT_GLOBALHEADER ) {
        // 部分封装要求 codec 配置放在容器头中，必须在打开编码器前设置该标志。
        m_codecContext->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }

    // 能力查询只是选参数，实际初始化仍可能因格式组合或构建选项失败。
    ret = avcodec_open2(m_codecContext, codec, nullptr);
    if ( ret < 0 ) {
        set_ffmpeg_error("Failed to open audio encoder", ret);
        return false;
    }

    m_stream->time_base = m_codecContext->time_base;
    // 编码器打开后可能生成额外配置数据，流参数必须在此时同步而不是提前复制。
    ret = avcodec_parameters_from_context(m_stream->codecpar, m_codecContext);
    // 参数复制不转移编码器上下文所有权，后续仍需分别回收 codec 与 format。
    if ( ret < 0 ) {
        set_ffmpeg_error("Failed to copy codec parameters", ret);
        return false;
    }

    // 复用包壳减少反复创建，每次写包后解除内容引用供下一轮使用。
    // 包只在流和编码上下文存活期间使用，close 先释放包再销毁其余上下文。
    m_packet = av_packet_alloc();
    if ( !m_packet ) {
        set_error("Failed to allocate encoder packet");
        return false;
    }

    if ( !(m_formatContext->oformat->flags & AVFMT_NOFILE) ) {
        // 只有需要显式文件句柄的 muxer 才打开 AVIO，NOFILE 路径由容器自身处理。
        ret = avio_open(
            &m_formatContext->pb, outputPath.c_str(), AVIO_FLAG_WRITE);
        if ( ret < 0 ) {
            set_ffmpeg_error("Failed to open output file", ret);
            return false;
        }
    }

    // 从此刻起磁盘可能已有头部；之后重采样器失败也不能恢复原文件内容。
    ret = avformat_write_header(m_formatContext, nullptr);
    if ( ret < 0 ) {
        set_ffmpeg_error("Failed to write output header", ret);
        return false;
    }

    if ( !open_resampler() ) {
        // 尚未发布 m_opened，因此失败清理不会试图对未就绪链路补写尾部。
        return false;
    }

    // 编码器、容器头和输入转换全部就绪后才允许写块与正常排尾。
    m_opened = true;
    return true;
}

/// @brief 建立平面 float 输入到实际编码格式的转换器及样本 FIFO。
/// @return 转换器与 FIFO 都可用时返回 true。
/// @warning 离线初始化：需要已打开的编码上下文，可能分配转换状态和缓存。
// 相同采样率仍可能需要样本格式或声道转换，因此不会直接跳过重采样器配置。
bool FFmpegFileReceiver::open_resampler()
{
    AVChannelLayout inputLayout{};
    // 引擎输入只给声道数，这里采用默认布局，不支持从 AudioBuffer
    // 传入自定义声道映射。
    av_channel_layout_default(&inputLayout,
                              static_cast<int>(m_format.channels));

    // 编码器的输出格式可能是交错或整数，但引擎输入固定描述为平面 float。
    int ret = swr_alloc_set_opts2(&m_swrContext,
                                  &m_codecContext->ch_layout,
                                  m_codecContext->sample_fmt,
                                  m_codecContext->sample_rate,
                                  &inputLayout,
                                  AV_SAMPLE_FMT_FLTP,
                                  static_cast<int>(m_format.samplerate),
                                  0,
                                  nullptr);
    av_channel_layout_uninit(&inputLayout);
    // 临时布局可立即释放，后续转换只依赖已交给 swr 的配置状态。
    if ( ret < 0 || !m_swrContext ) {
        set_ffmpeg_error("Failed to allocate audio resampler", ret);
        return false;
    }

    // 分配上下文和初始化滤波状态是两个失败点，不能把非空句柄等同于可转换。
    ret = swr_init(m_swrContext);
    if ( ret < 0 ) {
        set_ffmpeg_error("Failed to initialize audio resampler", ret);
        return false;
    }

    // FIFO 存储转换后的格式与声道，不按原输入类型解释其数据。
    // 初始容量只作起点，输入/输出采样率不同时后续写入仍可能扩容。
    m_fifo = av_audio_fifo_alloc(m_codecContext->sample_fmt,
                                 m_codecContext->ch_layout.nb_channels,
                                 static_cast<int>(m_blockFrames));
    // 初始容量来自 m_blockFrames 的 int 转换，上游 setter
    // 没有检查该窄化的上界。
    if ( !m_fifo ) {
        set_error("Failed to allocate encoder audio FIFO");
        return false;
    }

    return true;
}

/// @brief 转换一个输入块，将产出样本入队并提交可编码的完整帧。
/// @param buffer 与接收端输入格式一致的平面浮点缓冲，至少包含指定有效帧。
/// @param frame_count 输入采样率下的有效帧数，不是输出编码帧长。
/// @return 转换和入队成功时返回 true，不保证本块已经产生文件输出包。
/// @warning 离线逐块路径：分配样本数组与声道指针表，不能用于实时音频回调。
bool FFmpegFileReceiver::write_buffer(const AudioBuffer& buffer,
                                      std::size_t        frame_count)
{
    if ( !m_swrContext || !m_fifo || !m_codecContext ) {
        set_error("FFmpegFileReceiver is not open");
        return false;
    }
    if ( frame_count >
         static_cast<std::size_t>(std::numeric_limits<int>::max()) ) {
        // FFmpeg 此接口用 int 表示样本数，先阻止 size_t 窄化造成错误长度。
        set_error("Audio block is too large for FFmpeg");
        return false;
    }

    const float* const* input = buffer.raw_ptrs();
    // 这里只检查指针存在，缓冲实际长度和格式一致性由调用链保证。
    // 缓冲缺失不以静音替代，否则会把上游状态错误伪装为正常音频导出。
    if ( !input ) {
        set_error("Invalid audio buffer");
        return false;
    }
    for ( uint16_t channel = 0; channel < m_format.channels; ++channel ) {
        // 每个输入平面都必须有效，不能用空指针隐式表示某一声道静音。
        if ( !input[channel] ) {
            set_error("Invalid audio channel buffer");
            return false;
        }
    }

    const int inputFrames = static_cast<int>(frame_count);
    // 查询包含当前转换器延迟在内的容量上界，不把输入帧数直接作为输出容量。
    const int outputCapacity = swr_get_out_samples(m_swrContext, inputFrames);
    // 负查询结果不能转成无符号分配大小，以免把错误当作巨型样本容量。
    if ( outputCapacity < 0 ) {
        set_ffmpeg_error("Failed to query resampler output samples",
                         outputCapacity);
        return false;
    }
    if ( outputCapacity == 0 ) {
        // 当前分支直接成功返回且不调用
        // swr_convert，不能把它描述为已实际消费输入。
        return true;
    }

    SampleArray convertedSamples;
    // 样本区交给局部 RAII 对象，转换、入 FIFO 或编码失败都能自动回收。
    int ret = av_samples_alloc_array_and_samples(
        &convertedSamples.data,
        nullptr,
        m_codecContext->ch_layout.nb_channels,
        outputCapacity,
        m_codecContext->sample_fmt,
        0);
    if ( ret < 0 ) {
        set_ffmpeg_error("Failed to allocate converted samples", ret);
        return false;
    }

    // 只创建字节视图，不复制音频样本；原缓冲必须在同步转换返回前保持有效。
    std::vector<const uint8_t*> inputData(m_format.channels, nullptr);
    for ( uint16_t channel = 0; channel < m_format.channels; ++channel ) {
        inputData[channel] = reinterpret_cast<const uint8_t*>(input[channel]);
    }

    const int convertedFrames = swr_convert(m_swrContext,
                                            convertedSamples.data,
                                            outputCapacity,
                                            inputData.data(),
                                            inputFrames);
    // 容量是预留上界，实际有效帧数只由转换返回值决定，不能整块容量直接入队。
    if ( convertedFrames < 0 ) {
        set_ffmpeg_error("Failed to convert audio samples", convertedFrames);
        return false;
    }
    if ( convertedFrames == 0 ) {
        // 正常转换可能把输入保留在内部延迟中，零输出不等于编码结束或输入无效。
        return true;
    }

    if ( !write_converted_to_fifo(convertedSamples.data, convertedFrames) ) {
        return false;
    }
    // 非结束状态保留不足固定编码帧长的尾样本，等待后续输入拼成完整帧。
    return encode_fifo(false);
}

/// @brief 复制转换后的样本到 FIFO，调用后不保留输入数组所有权。
/// @param converted_data 按编码器格式组织的平面或交错样本指针。
/// @param frame_count 输出采样率下的帧数。
/// @return 成功入队或无有效输入时返回 true；短写也作为失败报告。
/// @warning 离线路径：FIFO 可能扩容，当前累加容量没有额外的 int 溢出检查。
bool FFmpegFileReceiver::write_converted_to_fifo(uint8_t** converted_data,
                                                 int       frame_count)
{
    if ( !m_fifo || !converted_data || frame_count <= 0 ) {
        // 缺 FIFO
        // 与空输入都按无操作成功处理；正常写块入口会先检查链路是否打开。
        return true;
    }

    int ret =
        // 保留原有残留样本，并为本次转换结果一起预留容量。
        av_audio_fifo_realloc(m_fifo, av_audio_fifo_size(m_fifo) + frame_count);
    if ( ret < 0 ) {
        set_ffmpeg_error("Failed to resize encoder audio FIFO", ret);
        return false;
    }

    // 写入复制输出格式样本，不持有转换数组的所有权；返回后局部数组可安全释放。
    ret = av_audio_fifo_write(
        m_fifo, reinterpret_cast<void**>(converted_data), frame_count);
    if ( ret < frame_count ) {
        // 短写会丢失本次提交的完整性，不继续假装所有输出都已进入 FIFO。
        if ( ret < 0 ) {
            set_ffmpeg_error("Failed to write encoder audio FIFO", ret);
        } else {
            // 非负短写没有 FFmpeg 错误码，单独给出不完整入队的诊断。
            set_error("Failed to write all converted samples to FIFO");
        }
        return false;
    }

    return true;
}

/// @brief 按编码器要求从 FIFO 取样本，构造帧并同步发送。
/// @param flush 结束阶段允许提交不足固定帧长的最后一帧。
/// @return 所有可提交样本处理成功时返回 true，缺链路时当前实现也返回 true。
/// @warning 离线编码路径：逐帧分配 AVFrame 与样本区，发送后可能进行文件写入。
bool FFmpegFileReceiver::encode_fifo(bool flush)
{
    if ( !m_fifo || !m_codecContext ) {
        return true;
    }

    const int codecFrameSize = m_codecContext->frame_size;
    // FIFO 的数量均为输出采样率下每声道帧数，不能与 m_framesWritten
    // 输入计数混用。
    while ( av_audio_fifo_size(m_fifo) > 0 ) {
        const int availableFrames = av_audio_fifo_size(m_fifo);
        if ( !flush && codecFrameSize > 0 &&
             availableFrames < codecFrameSize ) {
            // 固定帧长编码器在普通写入时等待补齐，不能提前排出每块不足长度的尾部。
            break;
        }

        // 可变帧长使用当前全部数据；结束阶段固定帧长取剩余与帧长的较小值。
        // 此处不补零，短尾帧能否接受由编码器处理，不在这里伪造额外输入样本。
        const int frameSamples =
            codecFrameSize > 0
                ? (flush ? std::min(availableFrames, codecFrameSize)
                         : codecFrameSize)
                : availableFrames;
        // 非正样本长度不能进入分配与 PTS 推进，也不应被替换成一帧静音提交。
        if ( frameSamples <= 0 ) {
            break;
        }

        // 自定义删除器统一释放帧及其样本引用，任何失败分支都无需手动重复清理。
        std::unique_ptr<AVFrame, AVFrameDeleter> frame(av_frame_alloc());
        if ( !frame ) {
            set_error("Failed to allocate audio frame");
            return false;
        }

        frame->nb_samples = frameSamples;
        frame->format     = m_codecContext->sample_fmt;
        // 每个新 AVFrame 显式设置格式属性，不依赖上一次帧的状态或默认采样配置。
        frame->sample_rate = m_codecContext->sample_rate;
        frame->pts         = m_nextPts;
        // 累计 PTS 没有额外溢出检查，超长输入预算仍需调用方约束。
        // 布局复制使该帧独立持有布局描述，不把编码上下文的内部指针转移出去。
        int ret = av_channel_layout_copy(&frame->ch_layout,
                                         &m_codecContext->ch_layout);
        if ( ret < 0 ) {
            set_ffmpeg_error("Failed to copy frame channel layout", ret);
            return false;
        }

        ret = av_frame_get_buffer(frame.get(), 0);
        // AVFrame 非空只表示帧壳存在，样本区要到这次分配成功才可写入。
        // 在确定帧格式和样本数后才分配存储，避免分配结果与实际声道布局不一致。
        if ( ret < 0 ) {
            set_ffmpeg_error("Failed to allocate frame samples", ret);
            return false;
        }

        ret = av_audio_fifo_read(
            // 读取会移出 FIFO；后面发送失败时当前实现不把样本重新放回队列。
            m_fifo,
            reinterpret_cast<void**>(frame->data),
            frameSamples);
        if ( ret < frameSamples ) {
            if ( ret < 0 ) {
                set_ffmpeg_error("Failed to read encoder audio FIFO", ret);
            } else {
                set_error("Failed to read all samples from FIFO");
            }
            return false;
        }

        // FIFO 已读出但编码发送尚未完成，失败时不会把这些样本重新放回队列。
        m_nextPts += frameSamples;
        // FIFO 使用 frame->data 而非 extended_data，大量平面声道仍需另行验证。
        // PTS 先于发送推进，失败后不能从这个内部计数推断已成功写入的输出时长。
        if ( !send_frame(frame.get()) ) {
            return false;
        }
    }

    return true;
}

/// @brief 依次排出重采样延迟、FIFO 尾部和编码器包，再写入容器尾部。
/// @return 无需处理或所有收尾步骤成功时返回 true。
/// @warning
/// 离线耗时路径：可能多次分配、编码与写文件，不检查停止标记或提供时限。
/// @warning 失败不回滚已消耗样本或已写文件；调用者不能无条件重试整个收尾流程。
bool FFmpegFileReceiver::finish_encoding()
{
    if ( !m_opened || m_trailerWritten ) {
        // 未完成 open 的半初始化链路不排尾；成功写过 trailer 的链路也不重复提交
        // EOF。
        return true;
    }

    // 排尾不再触发输入进度回调，延迟样本不会追加到 m_framesWritten 的输入计数。
    while ( m_swrContext ) {
        // 此阶段不读取 m_stopRequested，开始正常收尾后 stop
        // 不能保证及时中止排尾。
        // 零新输入查询当前可排出的容量上界，不再从音频图拉取额外样本。
        const int outputCapacity = swr_get_out_samples(m_swrContext, 0);
        if ( outputCapacity < 0 ) {
            // 查询失败不能被当作没有尾样本，否则会静默截断输出并继续写
            // trailer。
            set_ffmpeg_error("Failed to query resampler delayed samples",
                             outputCapacity);
            return false;
        }
        if ( outputCapacity == 0 ) {
            // 容量已为零无需再分配转换数组，后面仍需处理 FIFO 和编码器缓存。
            break;
        }

        SampleArray convertedSamples;
        // 每轮只持有本次排尾缓存，入队后立即可释放，不跨轮累积拥有多个样本区。
        int ret = av_samples_alloc_array_and_samples(
            &convertedSamples.data,
            nullptr,
            m_codecContext->ch_layout.nb_channels,
            outputCapacity,
            m_codecContext->sample_fmt,
            0);
        if ( ret < 0 ) {
            set_ffmpeg_error("Failed to allocate resampler flush samples", ret);
            return false;
        }

        // nullptr 输入是结束转换输入的请求，不是一个有长度的静音块。
        // 输出仍按编码器样本格式组织，与普通 write_buffer 的转换结果相同。
        const int convertedFrames = swr_convert(
            m_swrContext, convertedSamples.data, outputCapacity, nullptr, 0);
        if ( convertedFrames < 0 ) {
            set_ffmpeg_error("Failed to flush audio resampler",
                             convertedFrames);
            return false;
        }
        if ( convertedFrames == 0 ) {
            // 容量只是上界，实际零输出才结束本轮排尾，避免反复分配却无法前进。
            break;
        }

        // 重采样尾部也先经过 FIFO
        // 分帧，不把任意长度转换结果直接交给固定帧长编码器。
        // 这里仍只提交完整帧，最后不足部分统一留给后面的 flush=true。
        if ( !write_converted_to_fifo(convertedSamples.data, convertedFrames) ||
             !encode_fifo(false) ) {
            return false;
        }
    }

    // 顺序不可交换：先把 FIFO 的最后样本送完，再发空帧结束编码器输入。
    // 任一前序失败便短路，不继续把不完整编码链路标记为正常完成。
    if ( !encode_fifo(true) || !send_frame(nullptr) ) {
        return false;
    }

    // 只有编码器已完成本次 drain
    // 才写容器索引/尾部；它与关闭文件句柄是不同步骤。
    // 尾部失败会留下错误，使 close 跳过自动重试，不在未知容器状态下重复写尾。
    const int ret = av_write_trailer(m_formatContext);
    if ( ret < 0 ) {
        set_ffmpeg_error("Failed to write output trailer", ret);
        return false;
    }

    // 必须在 trailer 写成功后登记，close 才能避免再次尝试结束已完成的容器。
    // 此标记不代表数据经过操作系统级持久化屏障，也不是线程间完成通知。
    m_trailerWritten = true;
    return true;
}

/// @brief 提交一帧后立即尽可能取出当前编码包。
/// @param frame 已按编码器格式准备的帧；nullptr 表示不再提供输入并排出延迟包。
/// @return 提交及取包成功时返回 true，无编码上下文时当前实现也返回 true。
/// @warning 离线编码路径：可能编码和写文件，不保留调用者帧指针供异步任务使用。
/// @warning send 的负返回值全部按失败处理，没有针对 EAGAIN 先 drain
/// 后重试的分支。
bool FFmpegFileReceiver::send_frame(AVFrame* frame)
{
    if ( !m_codecContext ) {
        // 缺上下文被当作无操作；正常入口依赖 open
        // 建立的不变量，而非在此强制校验。
        return true;
    }

    // 发送成功只说明编码器接收了输入，一帧可能暂时不产生包，也可能产生多个包。
    const int ret = avcodec_send_frame(m_codecContext, frame);
    // 空帧也是一次发送状态转换，不能在已经结束的编码器上无条件重复提交。
    if ( ret < 0 ) {
        // 包括 EAGAIN
        // 在内均留下诊断并退出，不能使用取包终止规则忽略这里的失败。
        set_ffmpeg_error("Failed to send frame to encoder", ret);
        return false;
    }
    // 及时取空输出以便后续输入继续提交，不假定一次发送只对应一次 receive。
    return drain_packets();
}

/// @brief 取出当前可用的编码包，换算时间基并写入唯一音频流。
/// @return 遇到 EAGAIN/EOF 时返回 true，编码或封装写入错误返回 false。
/// @warning 离线 IO
/// 路径：循环次数由编码器输出决定，不检查停止标记或限制执行时间。
/// @warning 依赖 codec、stream 与 format
/// 上下文已就绪；这里只单独验证复用包存在。
bool FFmpegFileReceiver::drain_packets()
{
    if ( !m_packet ) {
        // 没有复用包无法接收编码结果，不能把资源未初始化解释成暂无输出。
        set_error("Encoder packet is not allocated");
        return false;
    }

    // 包写成功只表示 muxer 接受数据，不等于文件已持久化或重新解码验证通过。
    while ( true ) {
        // 持续取包直到本轮无数据或编码器 EOF，不用硬编码包数猜测编码延迟。
        int ret = avcodec_receive_packet(m_codecContext, m_packet);
        if ( is_packet_drain_finished(ret) ) {
            // EAGAIN 表示下次输入后还可继续，EOF
            // 则表示编码器已结束；二者都非文件写入错误。
            return true;
        }
        if ( ret < 0 ) {
            // 其他负值才作为编码失败，错误信息与下方 muxer 写失败区分保存。
            set_ffmpeg_error("Failed to receive encoded packet", ret);
            return false;
        }

        // muxer 写头可能调整流时间基，必须使用当前流
        // time_base，不能直接复制样本计数。
        // 包内的时间与时长一起换算，避免重采样后的输出时间轴按输入采样率解释。
        // 一个编码帧与一个包不必一一对应，时间换算以包提供的 PTS/DTS 为准。
        av_packet_rescale_ts(
            m_packet, m_codecContext->time_base, m_stream->time_base);
        m_packet->stream_index = m_stream->index;
        // 即使只有一条音频流，也显式指定容器流索引，不依赖包分配时的默认值。
        ret = av_interleaved_write_frame(m_formatContext, m_packet);
        // 失败可能发生在已经写出部分文件之后，返回值不提供事务式磁盘回滚。
        // 无论写入成功或失败都解除本轮包引用，保留可复用包壳，避免失败路径滞留数据。
        av_packet_unref(m_packet);
        if ( ret < 0 ) {
            set_ffmpeg_error("Failed to write encoded packet", ret);
            return false;
        }
    }
}

/// @brief 用操作上下文和 FFmpeg 错误文本替换当前诊断。
/// @param prefix 非空的操作描述字符串。
/// @param code FFmpeg 返回的状态码，调用者负责判断它是否表示失败。
/// @warning 诊断路径会分配字符串，不同步保护读取者，也不累计完整错误历史。
void FFmpegFileReceiver::set_ffmpeg_error(const char* prefix, int code)
{
    // 只保存最后一次错误，后续 set_error
    // 可覆盖；此函数本身不停止循环或释放资源。
    m_errorMessage = std::string(prefix) + ": " + ffmpeg_error_string(code);
}

/// @brief 替换没有直接 FFmpeg 错误码的失败描述。
/// @param message 要保存的诊断文本，移动进成员以复用调用方字符串存储。
/// @warning 与 error_message
/// 的引用读取不能无同步并发，调用者需保证对象线程归属。
void FFmpegFileReceiver::set_error(std::string message)
{
    // 统一保存错误供 start/open/close
    // 决策，设置文本不等于文件已关闭或尾部已写完。
    // 错误赋值不发布原子完成状态，读取线程必须通过外部同步保证字符串没有并发修改。
    m_errorMessage = std::move(message);
}

}  // namespace ice
