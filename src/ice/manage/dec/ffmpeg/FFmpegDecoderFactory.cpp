#include <fmt/format.h>

#include <ice/execptions/load_error.hpp>
#include <ice/manage/dec/ffmpeg/FFmpegDecoderFactory.hpp>

#include "ice/manage/dec/ffmpeg/FFmpegDecoderInstance.hpp"

extern "C" {
#include <libavformat/avformat.h>
}

#define AVCALL_CHECK(func)                          \
    if (int ret = func < 0) {                       \
        char errbuf[AV_ERROR_MAX_STRING_SIZE];      \
        av_strerror(ret, errbuf, sizeof(errbuf));   \
        throw ice::load_error(std::string(errbuf)); \
    }

namespace ice {
class FFmpegFormat {
   public:
    explicit FFmpegFormat(std::string_view file) {
        const std::string path_str(file);
        // 打开文件
        AVCALL_CHECK(
            avformat_open_input(&fmt_ctx, path_str.c_str(), nullptr, nullptr))
    }
    ~FFmpegFormat() { avformat_close_input(&fmt_ctx); }
    FFmpegFormat(const FFmpegFormat&) = delete;
    FFmpegFormat& operator=(const FFmpegFormat&) = delete;

    inline AVFormatContext* get() const { return fmt_ctx; }

   private:
    AVFormatContext* fmt_ctx{nullptr};
};

// 探测文件元信息
void FFmpegDecoderFactory::probe(std::string_view file_path,
                                 AudioDataFormat& format,
                                 size_t& total_frames) const {
    FFmpegFormat fffmt(file_path);
    AVCALL_CHECK(avformat_find_stream_info(fffmt.get(), nullptr))

    // find audio stream
    int audio_stream_index{-1};
    for (int i = 0; i < fffmt.get()->nb_streams; i++) {
        // codecpar stores decoder info
        if (fffmt.get()->streams[i]->codecpar->codec_type ==
            AVMEDIA_TYPE_AUDIO) {
            audio_stream_index = i;
            break;
        }
    }
    if (audio_stream_index == -1) {
        fmt::print("there\'s no audio stream");
        format.channels = 0;
        throw ice::load_error("find stream failed");
    }

    // 获取音频流的参数
    auto& audio_stream = fffmt.get()->streams[audio_stream_index];
    auto& codec_params = audio_stream->codecpar;

    format.channels = codec_params->ch_layout.nb_channels;
    format.samplerate = codec_params->sample_rate;

    // 获取总采样数
    if (audio_stream->duration > 0) {
        // 转换:总帧数 = 时长 * 采样率 / 时间基
        total_frames =
            av_rescale_q(audio_stream->duration, audio_stream->time_base,
                         {1, (int)format.samplerate});
    } else {
        // 某些容器格式可能在顶层 context 中有 duration
        if (fffmt.get()->duration > 0) {
            // AV_TIME_BASE_Q
            total_frames =
                av_rescale_q(fffmt.get()->duration, {1, AV_TIME_BASE},
                             {1, (int)format.samplerate});
        } else {
            fmt::print("file {} duration unknown", file_path);
            // 表示未知
            total_frames = 0;
        }
    }
}

// 创建解码器实例
std::unique_ptr<IDecoderInstance> FFmpegDecoderFactory::create_instance(
    std::string_view file_path) const {
    return std::make_unique<FFmpegDecoderInstance>(file_path);
}

}  // namespace ice
