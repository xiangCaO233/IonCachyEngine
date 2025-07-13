#include <fmt/format.h>

#include <ice/manage/dec/ffmpeg/FFmpegDecoderFactory.hpp>
#include <ice/manage/dec/ffmpeg/FFmpegDecoderInstance.hpp>

#include "ice/execptions/load_error.hpp"
#include "ice/manage/AudioFormat.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/codec.h>
#include <libavformat/avformat.h>
}

#define AVCALL_CHECK(func)                          \
    if (int ret = func < 0) {                       \
        char errbuf[AV_ERROR_MAX_STRING_SIZE];      \
        av_strerror(ret, errbuf, sizeof(errbuf));   \
        throw ice::load_error(std::string(errbuf)); \
    }

namespace ice {
// 完整定义
class FFmpegDecoder {
   public:
    explicit FFmpegDecoder(std::string_view file_path) {
        // 打开文件
        AVCALL_CHECK(
            avformat_open_input(&avfmt_ctx, file_path.data(), nullptr, nullptr))

        // 查找音频流
        for (int i = 0; i < avfmt_ctx->nb_streams; i++) {
            // codecpar 存储了解码器信息
            if (avfmt_ctx->streams[i]->codecpar->codec_type ==
                AVMEDIA_TYPE_AUDIO) {
                stream_index = i;
                break;
            }
        }
        if (stream_index == -1) {
            fmt::print("there\'s no audio stream");
            ice_format.channels = 0;
            throw ice::load_error("find stream failed");
        }

        // 获取音频流的参数
        auto& audio_stream = avfmt_ctx->streams[stream_index];
        auto& codec_params = audio_stream->codecpar;

        ice_format.channels = codec_params->ch_layout.nb_channels;
        ice_format.samplerate = codec_params->sample_rate;

        // 获取总采样数
        if (audio_stream->duration > 0) {
            // 转换:总帧数 = 时长 * 采样率 / 时间基
            total_frames =
                av_rescale_q(audio_stream->duration, audio_stream->time_base,
                             {1, (int)ice_format.samplerate});
        } else {
            // 某些容器格式可能在顶层 context 中有 duration
            if (avfmt_ctx->duration > 0) {
                // AV_TIME_BASE_Q
                total_frames =
                    av_rescale_q(avfmt_ctx->duration, {1, AV_TIME_BASE},
                                 {1, (int)ice_format.samplerate});
            } else {
                fmt::print("file {} duration unknown", file_path);
                // 表示未知
                total_frames = 0;
            }
        }

        // 查找编解码器
        avcodec = avcodec_find_decoder(codec_params->codec_id);
        if (!avcodec) {
            throw ice::load_error("no decoder find.");
        }

        // 分配编解码器
        avcodec_ctx = avcodec_alloc_context3(avcodec);
        if (!avcodec_ctx) {
            throw ice::load_error("decoder_context_alloc failed.");
        }

        // 将流的参数拷贝到解码器上下文中
        // 它告诉解码器要处理的数据的采样率,声道,格式等信息。
        AVCALL_CHECK(avcodec_parameters_to_context(avcodec_ctx, codec_params))

        // 打开解码器，准备开始工作
        AVCALL_CHECK(avcodec_open2(avcodec_ctx, avcodec, nullptr))
    }
    ~FFmpegDecoder() {
        avformat_close_input(&avfmt_ctx);
        avcodec_free_context(&avcodec_ctx);
    }
    FFmpegDecoder(const FFmpegDecoder&) = delete;
    FFmpegDecoder& operator=(const FFmpegDecoder&) = delete;

    inline const AudioDataFormat& iceformat() const { return ice_format; }

    inline size_t frames() const { return total_frames; }

   private:
    AVFormatContext* avfmt_ctx{nullptr};
    AVCodecContext* avcodec_ctx{nullptr};
    const AVCodec* avcodec{nullptr};
    AudioDataFormat ice_format;
    size_t total_frames;
    int stream_index;
};

FFmpegDecoderInstance::FFmpegDecoderInstance(std::string_view file_path) {
    // 初始化解码器资源
    ffimpl = std::make_unique<FFmpegDecoder>(file_path);
}
FFmpegDecoderInstance::~FFmpegDecoderInstance() = default;

// 定位帧位置
bool FFmpegDecoderInstance::seek(size_t pos) { return true; }

// 读取一块数据
size_t FFmpegDecoderInstance::read(float** buffer, size_t chunksize) {
    return 0;
}

const AudioDataFormat& FFmpegDecoderInstance::get_format() const {
    return ffimpl->iceformat();
}
size_t FFmpegDecoderInstance::get_total_frames() const {
    return ffimpl->frames();
}

}  // namespace ice
