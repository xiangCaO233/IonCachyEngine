#include <fmt/format.h>

#include <ice/manage/dec/ffmpeg/FFmpegDecoderFactory.hpp>
#include <ice/manage/dec/ffmpeg/FFmpegDecoderInstance.hpp>

#include "ice/manage/AudioFormat.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

#define AVCALL_CHECK(func)                                     \
    if (int ret = func < 0) {                                  \
        char errbuf[AV_ERROR_MAX_STRING_SIZE];                 \
        av_strerror(ret, errbuf, sizeof(errbuf));              \
        fmt::print("at {}:{} \n", std::string(#func), errbuf); \
        return;                                                \
    }

namespace ice {
// 完整定义
class FFmpegDecoder {
   public:
    FFmpegDecoder(std::string_view file_path) {
        // 打开文件
        AVCALL_CHECK(avformat_open_input(&avfmt_ctx, file_path.data(), nullptr,
                                         nullptr));
    }
    ~FFmpegDecoder() {
        avformat_close_input(&avfmt_ctx);
        avcodec_free_context(&avcodec_ctx);
    }
    FFmpegDecoder(const FFmpegDecoder&) = delete;
    FFmpegDecoder& operator=(const FFmpegDecoder&) = delete;

    inline const AudioDataFormat& iceformat() const { return ice_format; }

    inline const size_t frames() const { return total_frames; }

   private:
    AVFormatContext* avfmt_ctx{nullptr};
    AVCodecContext* avcodec_ctx{nullptr};
    AudioDataFormat ice_format;
    size_t total_frames;
    int stream_index;
};

FFmpegDecoderInstance::FFmpegDecoderInstance(std::string_view file_path) {
    // 初始化解码器资源
    ffimpl = std::make_unique<FFmpegDecoder>(file_path);
}

// 需要声明析构函数-隐藏ffmpeg实现细节
FFmpegDecoderInstance::~FFmpegDecoderInstance() {}

// 定位帧位置
bool FFmpegDecoderInstance::seek(size_t pos) { return true; }

// 读取一块数据
size_t FFmpegDecoderInstance::read(float** buffer, size_t chunksize) {
    return chunksize;
}

const AudioDataFormat& FFmpegDecoderInstance::get_format() const {
    return ffimpl->iceformat();
}
size_t FFmpegDecoderInstance::get_total_frames() const {
    return ffimpl->frames();
}

}  // namespace ice
