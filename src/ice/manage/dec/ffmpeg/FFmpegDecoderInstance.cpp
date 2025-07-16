#include <fmt/base.h>
#include <fmt/format.h>

#include <ice/manage/dec/ffmpeg/FFmpegDecoderFactory.hpp>
#include <ice/manage/dec/ffmpeg/FFmpegDecoderInstance.hpp>

#include "ice/execptions/load_error.hpp"
#include "ice/manage/AudioBuffer.hpp"
#include "ice/manage/AudioFormat.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/codec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
}

#define AVCALL_CHECK(func)                          \
    if (int ret = func < 0) {                       \
        char errbuf[AV_ERROR_MAX_STRING_SIZE];      \
        av_strerror(ret, errbuf, sizeof(errbuf));   \
        throw ice::load_error(std::string(errbuf)); \
    }
#define AVCALL_CHECKRETB(func)                    \
    if (int ret = func < 0) {                     \
        char errbuf[AV_ERROR_MAX_STRING_SIZE];    \
        av_strerror(ret, errbuf, sizeof(errbuf)); \
        fmt::print("averr: {}", errbuf);          \
        return false;                             \
    }
#define AVCALL_CHECKRETV(func)                    \
    if (int ret = func < 0) {                     \
        char errbuf[AV_ERROR_MAX_STRING_SIZE];    \
        av_strerror(ret, errbuf, sizeof(errbuf)); \
        fmt::print("averr: {}", errbuf);          \
        return ret;                               \
    }

namespace ice {
// 完整定义
class FFmpegDecoder {
   public:
    explicit FFmpegDecoder(std::string_view file_path) {
        // 打开文件
        AVCALL_CHECK(
            avformat_open_input(&avfmt_ctx, file_path.data(), nullptr, nullptr))

        // 查找流信息(艾斯比mp3)
        AVCALL_CHECK(avformat_find_stream_info(avfmt_ctx, nullptr))

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
            // 总帧数 = 时长 * 采样率 / 时间基
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
                // 未知放0
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
        // 它告诉解码器要处理的数据的采样率,声道,格式等信息
        AVCALL_CHECK(avcodec_parameters_to_context(avcodec_ctx, codec_params))

        // 打开解码器
        AVCALL_CHECK(avcodec_open2(avcodec_ctx, avcodec, nullptr))
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
        AVCALL_CHECK(swr_alloc_set_opts2(
            // @param ps
            // Pointer to an existing Swr context if available,
            // or to NULL if not.
            // On success, *ps will be set to the allocated context.
            //
            &swr_ctx,
            &avcodec_ctx->ch_layout,  // 目标声道布局(不变)
            AV_SAMPLE_FMT_FLTP,  // 目标样本格式固定为planner-float(32位浮点数，平面)
            avcodec_ctx->sample_rate,  // 目标采样率(不变)
            &avcodec_ctx->ch_layout,   // 源声道布局
            avcodec_ctx->sample_fmt,   // 源样本格式
            avcodec_ctx->sample_rate,  // 源采样率
            0,                         // 日志偏移
            nullptr                    // 日志上下文
            ))

        if (!swr_ctx) {
            throw ice::load_error("swr_alloc_set_opts failed.");
        }

        // 初始化重采样器上下文
        AVCALL_CHECK(swr_init(swr_ctx))

        // 初始化转换缓冲区
        const size_t max_frames_per_avframe = 2048;
        conversion_buffer.resize({(uint16_t)avcodec_ctx->ch_layout.nb_channels,
                                  (uint32_t)avcodec_ctx->sample_rate},
                                 max_frames_per_avframe);
    }
    ~FFmpegDecoder() {
        // 释放
        av_frame_free(&avframe);
        av_packet_free(&avpacket);
        avcodec_free_context(&avcodec_ctx);
        avformat_close_input(&avfmt_ctx);
        if (swr_ctx) {
            swr_free(&swr_ctx);
        }
    }
    FFmpegDecoder(const FFmpegDecoder&) = delete;
    FFmpegDecoder& operator=(const FFmpegDecoder&) = delete;

    // 移动指针到指定帧位置
    bool seek_to_frame(size_t frame_offset) {
        // 将帧偏移量转换回FFmpeg的时间基单位
        int64_t timestamp =
            av_rescale_q(frame_offset, {1, avcodec_ctx->sample_rate},
                         avfmt_ctx->streams[stream_index]->time_base);

        // 执行寻道操作
        // AVSEEK_FLAG_BACKWARD 保证我们能seek到请求的关键帧或其之前最近的关键帧
        AVCALL_CHECKRETB(av_seek_frame(avfmt_ctx, stream_index, timestamp,
                                       AVSEEK_FLAG_BACKWARD))

        // 寻道后解码器内部的状态可能是不一致的,需要刷新
        avcodec_flush_buffers(avcodec_ctx);
        return true;
    }

    // 解码数据
    size_t decode(float** buffer, size_t chunksize) {
        size_t frames_decoded_total = 0;

        while (frames_decoded_total < chunksize) {
            // 步骤 1: 优先处理上一次遗留的数据
            if (conversion_buffer_remains > 0) {
                size_t frames_to_copy =
                    std::min(chunksize - frames_decoded_total,
                             conversion_buffer_remains);

                for (uint16_t ch = 0; ch < avcodec_ctx->ch_layout.nb_channels;
                     ++ch) {
                    const float* src = conversion_buffer.raw_ptrs()[ch] +
                                       conversion_buffer_offset;
                    float* dest = buffer[ch] + frames_decoded_total;
                    memcpy(dest, src, frames_to_copy * sizeof(float));
                }

                frames_decoded_total += frames_to_copy;
                conversion_buffer_remains -= frames_to_copy;
                conversion_buffer_offset += frames_to_copy;

                if (frames_decoded_total >= chunksize) break;
            }

            // 如果遗留数据已用完，重置偏移
            conversion_buffer_offset = 0;

            // 从解码器获取一个新的原生帧
            int ret = avcodec_receive_frame(avcodec_ctx, avframe);

            if (ret == 0) {
                // 将整个原生帧转换到的内部缓冲区
                int converted_count = swr_convert(
                    swr_ctx, (uint8_t**)conversion_buffer.raw_ptrs(),
                    conversion_buffer.num_frames(),
                    (const uint8_t**)avframe->data, avframe->nb_samples);

                if (converted_count > 0) {
                    // 更新我们内部缓冲区的剩余帧数
                    conversion_buffer_remains = converted_count;
                }
                av_frame_unref(avframe);

                continue;
            } else if (ret == AVERROR(EAGAIN)) {
                // 发送新Packet
                av_packet_unref(avpacket);
                int read_ret = av_read_frame(avfmt_ctx, avpacket);
                if (read_ret == AVERROR_EOF) {
                    // 发送空pack
                    avcodec_send_packet(avcodec_ctx, nullptr);
                    // 不需要continue，下一次while循环会处理解码器的EOF
                } else if (read_ret < 0) {
                    // 读文件出错，终止
                    break;
                } else if (avpacket->stream_index == stream_index) {
                    avcodec_send_packet(avcodec_ctx, avpacket);
                }
            } else if (ret == AVERROR_EOF) {
                // 解码器已被完全清空，没有更多帧了
                // 冲洗(flush)重采样器，获取它内部缓冲的最后几帧
                int flushed_count;
                do {
                    // 先冲洗到临时缓冲区
                    flushed_count = swr_convert(
                        swr_ctx, (uint8_t**)conversion_buffer.raw_ptrs(),
                        conversion_buffer.num_frames(), nullptr, 0);
                    if (flushed_count > 0) {
                        // 再从临时缓冲区拷贝到最终位置
                        size_t frames_needed = chunksize - frames_decoded_total;
                        size_t frames_to_copy =
                            std::min((size_t)flushed_count, frames_needed);
                        for (uint16_t ch = 0;
                             ch < avcodec_ctx->ch_layout.nb_channels; ++ch) {
                            const float* src = conversion_buffer.raw_ptrs()[ch];
                            // 写入到 buffer 的正确偏移位置
                            float* dest = buffer[ch] + frames_decoded_total;
                            memcpy(dest, src, frames_to_copy * sizeof(float));
                        }
                        frames_decoded_total += frames_to_copy;
                    }
                } while (flushed_count > 0);
                // 到达文件末尾，跳出整个while循环
                break;
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
    AVFormatContext* avfmt_ctx{nullptr};
    AVCodecContext* avcodec_ctx{nullptr};
    const AVCodec* avcodec{nullptr};
    AVPacket* avpacket{nullptr};
    AVFrame* avframe{nullptr};
    SwrContext* swr_ctx{nullptr};
    AudioDataFormat ice_format;
    AudioBuffer conversion_buffer;
    // 新增：记录上一次转换后，在 conversion_buffer 中还剩下多少帧
    size_t conversion_buffer_remains = 0;
    // 新增：记录上一次转换后，我们从缓冲区的哪个位置开始拷贝的
    size_t conversion_buffer_offset = 0;
    size_t total_frames;
    int stream_index;
};

FFmpegDecoderInstance::FFmpegDecoderInstance(std::string_view file_path) {
    // 初始化解码器资源
    ffimpl = std::make_unique<FFmpegDecoder>(file_path);
}
FFmpegDecoderInstance::~FFmpegDecoderInstance() = default;

// 定位帧位置
bool FFmpegDecoderInstance::seek(size_t pos) {
    return ffimpl->seek_to_frame(pos);
}

// 读取一块数据
size_t FFmpegDecoderInstance::read(float** buffer, size_t chunksize) {
    return ffimpl->decode(buffer, chunksize);
}

const AudioDataFormat& FFmpegDecoderInstance::get_format() const {
    return ffimpl->iceformat();
}
size_t FFmpegDecoderInstance::get_total_frames() const {
    return ffimpl->frames();
}

}  // namespace ice
