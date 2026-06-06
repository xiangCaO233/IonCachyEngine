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
struct OutputFormatSelection {
    /// @brief 显式指定的 FFmpeg muxer 名称；nullptr 表示按路径自动推断。
    const char* formatName{ nullptr };

    /// @brief 显式指定的编码器；AV_CODEC_ID_NONE 表示使用 muxer 默认值。
    AVCodecID codecOverride{ AV_CODEC_ID_NONE };
};

/// @brief 将文件系统路径转换为 FFmpeg 使用的 UTF-8 字符串。
/// @param path 文件系统路径。
/// @return UTF-8 路径字符串。
std::string path_to_utf8(const std::filesystem::path& path)
{
    auto u8Path = path.u8string();
    return std::string(reinterpret_cast<const char*>(u8Path.c_str()),
                       u8Path.size());
}

/// @brief 转为小写扩展名。
/// @param path 输出路径。
/// @return 小写扩展名。
std::string lowercase_extension(const std::filesystem::path& path)
{
    std::string extension = path_to_utf8(path.extension());
    std::transform(
        extension.begin(),
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
    const std::string extension = lowercase_extension(outputPath);
    if ( extension == ".m4a" ) {
        return OutputFormatSelection{ "mp4", AV_CODEC_ID_AAC };
    }
    if ( extension == ".opus" ) {
        return OutputFormatSelection{ "ogg", AV_CODEC_ID_OPUS };
    }
    return OutputFormatSelection{};
}

/// @brief 将 FFmpeg 错误码转换为文本。
/// @param code FFmpeg 错误码。
/// @return 错误文本。
std::string ffmpeg_error_string(int code)
{
    char buffer[AV_ERROR_MAX_STRING_SIZE] = { 0 };
    av_strerror(code, buffer, sizeof(buffer));
    return buffer;
}

/// @brief 判断错误码是否表示编码器暂时无包可取。
/// @param code FFmpeg 错误码。
/// @return 需要停止 drain 但不是失败时返回 true。
bool is_packet_drain_finished(int code)
{
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
        return true;
    }

    const auto* formats = static_cast<const AVSampleFormat*>(configs);
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
    constexpr AVSampleFormat preferredFormats[] = {
        AV_SAMPLE_FMT_FLTP, AV_SAMPLE_FMT_FLT,  AV_SAMPLE_FMT_S16P,
        AV_SAMPLE_FMT_S16,  AV_SAMPLE_FMT_S32P, AV_SAMPLE_FMT_S32,
        AV_SAMPLE_FMT_DBLP, AV_SAMPLE_FMT_DBL,  AV_SAMPLE_FMT_NONE,
    };

    for ( const AVSampleFormat format : preferredFormats ) {
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
        return static_cast<const AVSampleFormat*>(configs)[0];
    }
    return AV_SAMPLE_FMT_FLTP;
}

/// @brief 选择编码器支持的采样率。
/// @param codec 编码器。
/// @param desiredSampleRate 期望采样率。
/// @return 采样率。
int select_sample_rate(const AVCodec* codec, std::uint32_t desiredSampleRate)
{
    const auto  desired     = static_cast<int>(desiredSampleRate);
    const void* configs     = nullptr;
    int         configCount = 0;
    const int   configRet   = avcodec_get_supported_config(
        nullptr, codec, AV_CODEC_CONFIG_SAMPLE_RATE, 0, &configs, &configCount);
    if ( configRet < 0 || !configs || configCount <= 0 ) {
        return desired;
    }

    const auto* sampleRates = static_cast<const int*>(configs);
    int         fallback    = 0;
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
        return AVERROR(EINVAL);
    }

    AVChannelLayout desiredLayout{};
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
        for ( int index = 0;
              index < configCount && layouts[index].nb_channels > 0;
              ++index ) {
            const AVChannelLayout* layout = &layouts[index];
            if ( !fallback ) {
                fallback = layout;
            }
            if ( layout->nb_channels == desiredChannels ) {
                const int ret = av_channel_layout_copy(output, layout);
                av_channel_layout_uninit(&desiredLayout);
                return ret;
            }
        }

        if ( fallback ) {
            const int ret = av_channel_layout_copy(output, fallback);
            av_channel_layout_uninit(&desiredLayout);
            return ret;
        }
    }

    const int ret = av_channel_layout_copy(output, &desiredLayout);
    av_channel_layout_uninit(&desiredLayout);
    return ret;
}

/// @brief 判断编码格式是否需要设置默认码率。
/// @param codecId 编码格式。
/// @return 需要设置时返回 true。
bool should_set_default_bitrate(AVCodecID codecId)
{
    return codecId == AV_CODEC_ID_MP3 || codecId == AV_CODEC_ID_AAC ||
           codecId == AV_CODEC_ID_VORBIS || codecId == AV_CODEC_ID_OPUS ||
           codecId == AV_CODEC_ID_WMAV1 || codecId == AV_CODEC_ID_WMAV2;
}

/// @brief AVFrame deleter。
struct AVFrameDeleter {
    /// @brief 释放 AVFrame。
    /// @param frame 要释放的帧。
    void operator()(AVFrame* frame) const { av_frame_free(&frame); }
};

/// @brief FFmpeg 采样数组的 RAII 包装。
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
        av_freep(&data[0]);
        av_freep(&data);
    }
};

}  // namespace

FFmpegFileReceiver::FFmpegFileReceiver(std::filesystem::path  output_path,
                                       const AudioDataFormat& format)
    : IReceiver(format), m_outputPath(std::move(output_path)), m_format(format)
{
    m_buffer.resize(m_format, m_blockFrames);
}

FFmpegFileReceiver::~FFmpegFileReceiver()
{
    close();
}

void FFmpegFileReceiver::set_target_frames(std::size_t frame_count)
{
    m_targetFrames = frame_count;
}

void FFmpegFileReceiver::set_block_frames(std::size_t frame_count)
{
    if ( frame_count == 0 ) {
        return;
    }
    m_blockFrames = frame_count;
    m_buffer.resize(m_format, m_blockFrames);
}

void FFmpegFileReceiver::set_progress_callback(
    std::function<void(std::size_t)> callback)
{
    m_progressCallback = std::move(callback);
}

std::size_t FFmpegFileReceiver::frames_written() const
{
    return m_framesWritten;
}

const std::string& FFmpegFileReceiver::error_message() const
{
    return m_errorMessage;
}

bool FFmpegFileReceiver::open()
{
    if ( m_opened ) {
        return true;
    }
    m_errorMessage.clear();
    m_framesWritten  = 0;
    m_nextPts        = 0;
    m_trailerWritten = false;
    m_stopRequested.store(false);

    if ( !open_encoder() ) {
        close();
        return false;
    }
    return true;
}

void FFmpegFileReceiver::close()
{
    if ( m_opened && !m_trailerWritten && m_errorMessage.empty() ) {
        static_cast<void>(finish_encoding());
    }

    if ( m_fifo ) {
        av_audio_fifo_free(m_fifo);
        m_fifo = nullptr;
    }
    if ( m_swrContext ) {
        swr_free(&m_swrContext);
    }
    if ( m_packet ) {
        av_packet_free(&m_packet);
    }
    if ( m_codecContext ) {
        avcodec_free_context(&m_codecContext);
    }
    if ( m_formatContext ) {
        if ( !(m_formatContext->oformat->flags & AVFMT_NOFILE) &&
             m_formatContext->pb ) {
            avio_closep(&m_formatContext->pb);
        }
        avformat_free_context(m_formatContext);
    }

    m_formatContext  = nullptr;
    m_codecContext   = nullptr;
    m_stream         = nullptr;
    m_nextPts        = 0;
    m_opened         = false;
    m_trailerWritten = false;
    m_running.store(false);
}

bool FFmpegFileReceiver::start()
{
    if ( m_running.load() ) {
        set_error("FFmpegFileReceiver is already running");
        return false;
    }
    if ( m_targetFrames == 0 ) {
        set_error("FFmpegFileReceiver target frame count is zero");
        return false;
    }
    if ( !m_opened && !open() ) {
        return false;
    }

    m_running.store(true);
    bool ok = true;

    while ( m_framesWritten < m_targetFrames && !m_stopRequested.load() ) {
        const std::size_t frameCount =
            std::min(m_blockFrames, m_targetFrames - m_framesWritten);
        m_buffer.resize(m_format, frameCount);
        m_buffer.clear();
        if ( get_source() ) {
            get_source()->process(m_buffer);
        }
        if ( !write_buffer(m_buffer, frameCount) ) {
            ok = false;
            break;
        }
        m_framesWritten += frameCount;
        if ( m_progressCallback ) {
            m_progressCallback(m_framesWritten);
        }
    }

    if ( m_stopRequested.load() && ok ) {
        set_error("FFmpegFileReceiver stopped");
        ok = false;
    }
    if ( ok && !finish_encoding() ) {
        ok = false;
    }

    m_running.store(false);
    close();
    return ok;
}

void FFmpegFileReceiver::stop()
{
    m_stopRequested.store(true);
}

bool FFmpegFileReceiver::is_running() const
{
    return m_running.load();
}

bool FFmpegFileReceiver::open_encoder()
{
    if ( m_format.channels == 0 || m_format.samplerate == 0 ) {
        set_error("Invalid FFmpegFileReceiver input format");
        return false;
    }

    const std::string           outputPath = path_to_utf8(m_outputPath);
    const OutputFormatSelection outputSelection =
        select_output_format(m_outputPath);
    int ret = avformat_alloc_output_context2(&m_formatContext,
                                             nullptr,
                                             outputSelection.formatName,
                                             outputPath.c_str());
    if ( ret < 0 || !m_formatContext || !m_formatContext->oformat ) {
        set_ffmpeg_error("Failed to allocate output context", ret);
        return false;
    }

    const AVCodecID codecId = outputSelection.codecOverride != AV_CODEC_ID_NONE
                                  ? outputSelection.codecOverride
                                  : m_formatContext->oformat->audio_codec;
    if ( codecId == AV_CODEC_ID_NONE ) {
        set_error("Output format does not provide a default audio encoder");
        return false;
    }

    const AVCodec* codec = avcodec_find_encoder(codecId);
    if ( !codec ) {
        set_error("FFmpeg encoder is not available for output format");
        return false;
    }

    m_stream = avformat_new_stream(m_formatContext, nullptr);
    if ( !m_stream ) {
        set_error("Failed to create audio stream");
        return false;
    }

    m_codecContext = avcodec_alloc_context3(codec);
    if ( !m_codecContext ) {
        set_error("Failed to allocate codec context");
        return false;
    }

    AVChannelLayout outputLayout{};
    ret = select_channel_layout(
        codec, static_cast<int>(m_format.channels), &outputLayout);
    if ( ret < 0 ) {
        set_ffmpeg_error("Failed to select encoder channel layout", ret);
        return false;
    }

    m_codecContext->codec_id   = codecId;
    m_codecContext->codec_type = AVMEDIA_TYPE_AUDIO;
    m_codecContext->sample_fmt = select_sample_format(codec);
    m_codecContext->sample_rate =
        select_sample_rate(codec, m_format.samplerate);
    m_codecContext->time_base = AVRational{ 1, m_codecContext->sample_rate };
    m_codecContext->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;
    if ( should_set_default_bitrate(codecId) ) {
        m_codecContext->bit_rate = 192000;
    }

    ret = av_channel_layout_copy(&m_codecContext->ch_layout, &outputLayout);
    av_channel_layout_uninit(&outputLayout);
    if ( ret < 0 ) {
        set_ffmpeg_error("Failed to copy encoder channel layout", ret);
        return false;
    }

    if ( m_formatContext->oformat->flags & AVFMT_GLOBALHEADER ) {
        m_codecContext->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }

    ret = avcodec_open2(m_codecContext, codec, nullptr);
    if ( ret < 0 ) {
        set_ffmpeg_error("Failed to open audio encoder", ret);
        return false;
    }

    m_stream->time_base = m_codecContext->time_base;
    ret = avcodec_parameters_from_context(m_stream->codecpar, m_codecContext);
    if ( ret < 0 ) {
        set_ffmpeg_error("Failed to copy codec parameters", ret);
        return false;
    }

    m_packet = av_packet_alloc();
    if ( !m_packet ) {
        set_error("Failed to allocate encoder packet");
        return false;
    }

    if ( !(m_formatContext->oformat->flags & AVFMT_NOFILE) ) {
        ret = avio_open(
            &m_formatContext->pb, outputPath.c_str(), AVIO_FLAG_WRITE);
        if ( ret < 0 ) {
            set_ffmpeg_error("Failed to open output file", ret);
            return false;
        }
    }

    ret = avformat_write_header(m_formatContext, nullptr);
    if ( ret < 0 ) {
        set_ffmpeg_error("Failed to write output header", ret);
        return false;
    }

    if ( !open_resampler() ) {
        return false;
    }

    m_opened = true;
    return true;
}

bool FFmpegFileReceiver::open_resampler()
{
    AVChannelLayout inputLayout{};
    av_channel_layout_default(&inputLayout,
                              static_cast<int>(m_format.channels));

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
    if ( ret < 0 || !m_swrContext ) {
        set_ffmpeg_error("Failed to allocate audio resampler", ret);
        return false;
    }

    ret = swr_init(m_swrContext);
    if ( ret < 0 ) {
        set_ffmpeg_error("Failed to initialize audio resampler", ret);
        return false;
    }

    m_fifo = av_audio_fifo_alloc(m_codecContext->sample_fmt,
                                 m_codecContext->ch_layout.nb_channels,
                                 static_cast<int>(m_blockFrames));
    if ( !m_fifo ) {
        set_error("Failed to allocate encoder audio FIFO");
        return false;
    }

    return true;
}

bool FFmpegFileReceiver::write_buffer(const AudioBuffer& buffer,
                                      std::size_t        frame_count)
{
    if ( !m_swrContext || !m_fifo || !m_codecContext ) {
        set_error("FFmpegFileReceiver is not open");
        return false;
    }
    if ( frame_count >
         static_cast<std::size_t>(std::numeric_limits<int>::max()) ) {
        set_error("Audio block is too large for FFmpeg");
        return false;
    }

    const float* const* input = buffer.raw_ptrs();
    if ( !input ) {
        set_error("Invalid audio buffer");
        return false;
    }
    for ( uint16_t channel = 0; channel < m_format.channels; ++channel ) {
        if ( !input[channel] ) {
            set_error("Invalid audio channel buffer");
            return false;
        }
    }

    const int inputFrames    = static_cast<int>(frame_count);
    const int outputCapacity = swr_get_out_samples(m_swrContext, inputFrames);
    if ( outputCapacity < 0 ) {
        set_ffmpeg_error("Failed to query resampler output samples",
                         outputCapacity);
        return false;
    }
    if ( outputCapacity == 0 ) {
        return true;
    }

    SampleArray convertedSamples;
    int         ret = av_samples_alloc_array_and_samples(
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

    std::vector<const uint8_t*> inputData(m_format.channels, nullptr);
    for ( uint16_t channel = 0; channel < m_format.channels; ++channel ) {
        inputData[channel] = reinterpret_cast<const uint8_t*>(input[channel]);
    }

    const int convertedFrames = swr_convert(m_swrContext,
                                            convertedSamples.data,
                                            outputCapacity,
                                            inputData.data(),
                                            inputFrames);
    if ( convertedFrames < 0 ) {
        set_ffmpeg_error("Failed to convert audio samples", convertedFrames);
        return false;
    }
    if ( convertedFrames == 0 ) {
        return true;
    }

    if ( !write_converted_to_fifo(convertedSamples.data, convertedFrames) ) {
        return false;
    }
    return encode_fifo(false);
}

bool FFmpegFileReceiver::write_converted_to_fifo(uint8_t** converted_data,
                                                 int       frame_count)
{
    if ( !m_fifo || !converted_data || frame_count <= 0 ) {
        return true;
    }

    int ret =
        av_audio_fifo_realloc(m_fifo, av_audio_fifo_size(m_fifo) + frame_count);
    if ( ret < 0 ) {
        set_ffmpeg_error("Failed to resize encoder audio FIFO", ret);
        return false;
    }

    ret = av_audio_fifo_write(
        m_fifo, reinterpret_cast<void**>(converted_data), frame_count);
    if ( ret < frame_count ) {
        if ( ret < 0 ) {
            set_ffmpeg_error("Failed to write encoder audio FIFO", ret);
        } else {
            set_error("Failed to write all converted samples to FIFO");
        }
        return false;
    }

    return true;
}

bool FFmpegFileReceiver::encode_fifo(bool flush)
{
    if ( !m_fifo || !m_codecContext ) {
        return true;
    }

    const int codecFrameSize = m_codecContext->frame_size;
    while ( av_audio_fifo_size(m_fifo) > 0 ) {
        const int availableFrames = av_audio_fifo_size(m_fifo);
        if ( !flush && codecFrameSize > 0 &&
             availableFrames < codecFrameSize ) {
            break;
        }

        const int frameSamples =
            codecFrameSize > 0
                ? (flush ? std::min(availableFrames, codecFrameSize)
                         : codecFrameSize)
                : availableFrames;
        if ( frameSamples <= 0 ) {
            break;
        }

        std::unique_ptr<AVFrame, AVFrameDeleter> frame(av_frame_alloc());
        if ( !frame ) {
            set_error("Failed to allocate audio frame");
            return false;
        }

        frame->nb_samples  = frameSamples;
        frame->format      = m_codecContext->sample_fmt;
        frame->sample_rate = m_codecContext->sample_rate;
        frame->pts         = m_nextPts;
        int ret            = av_channel_layout_copy(&frame->ch_layout,
                                                    &m_codecContext->ch_layout);
        if ( ret < 0 ) {
            set_ffmpeg_error("Failed to copy frame channel layout", ret);
            return false;
        }

        ret = av_frame_get_buffer(frame.get(), 0);
        if ( ret < 0 ) {
            set_ffmpeg_error("Failed to allocate frame samples", ret);
            return false;
        }

        ret = av_audio_fifo_read(
            m_fifo, reinterpret_cast<void**>(frame->data), frameSamples);
        if ( ret < frameSamples ) {
            if ( ret < 0 ) {
                set_ffmpeg_error("Failed to read encoder audio FIFO", ret);
            } else {
                set_error("Failed to read all samples from FIFO");
            }
            return false;
        }

        m_nextPts += frameSamples;
        if ( !send_frame(frame.get()) ) {
            return false;
        }
    }

    return true;
}

bool FFmpegFileReceiver::finish_encoding()
{
    if ( !m_opened || m_trailerWritten ) {
        return true;
    }

    while ( m_swrContext ) {
        const int outputCapacity = swr_get_out_samples(m_swrContext, 0);
        if ( outputCapacity < 0 ) {
            set_ffmpeg_error("Failed to query resampler delayed samples",
                             outputCapacity);
            return false;
        }
        if ( outputCapacity == 0 ) {
            break;
        }

        SampleArray convertedSamples;
        int         ret = av_samples_alloc_array_and_samples(
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

        const int convertedFrames = swr_convert(
            m_swrContext, convertedSamples.data, outputCapacity, nullptr, 0);
        if ( convertedFrames < 0 ) {
            set_ffmpeg_error("Failed to flush audio resampler",
                             convertedFrames);
            return false;
        }
        if ( convertedFrames == 0 ) {
            break;
        }

        if ( !write_converted_to_fifo(convertedSamples.data, convertedFrames) ||
             !encode_fifo(false) ) {
            return false;
        }
    }

    if ( !encode_fifo(true) || !send_frame(nullptr) ) {
        return false;
    }

    const int ret = av_write_trailer(m_formatContext);
    if ( ret < 0 ) {
        set_ffmpeg_error("Failed to write output trailer", ret);
        return false;
    }

    m_trailerWritten = true;
    return true;
}

bool FFmpegFileReceiver::send_frame(AVFrame* frame)
{
    if ( !m_codecContext ) {
        return true;
    }

    const int ret = avcodec_send_frame(m_codecContext, frame);
    if ( ret < 0 ) {
        set_ffmpeg_error("Failed to send frame to encoder", ret);
        return false;
    }
    return drain_packets();
}

bool FFmpegFileReceiver::drain_packets()
{
    if ( !m_packet ) {
        set_error("Encoder packet is not allocated");
        return false;
    }

    while ( true ) {
        int ret = avcodec_receive_packet(m_codecContext, m_packet);
        if ( is_packet_drain_finished(ret) ) {
            return true;
        }
        if ( ret < 0 ) {
            set_ffmpeg_error("Failed to receive encoded packet", ret);
            return false;
        }

        av_packet_rescale_ts(
            m_packet, m_codecContext->time_base, m_stream->time_base);
        m_packet->stream_index = m_stream->index;
        ret = av_interleaved_write_frame(m_formatContext, m_packet);
        av_packet_unref(m_packet);
        if ( ret < 0 ) {
            set_ffmpeg_error("Failed to write encoded packet", ret);
            return false;
        }
    }
}

void FFmpegFileReceiver::set_ffmpeg_error(const char* prefix, int code)
{
    m_errorMessage = std::string(prefix) + ": " + ffmpeg_error_string(code);
}

void FFmpegFileReceiver::set_error(std::string message)
{
    m_errorMessage = std::move(message);
}

}  // namespace ice
