#include <ice/manage/dec/ffmpeg/FFmpegDecoderFactory.hpp>

namespace ice {

// 探测文件元信息的通用接口
void FFmpegDecoderFactory::probe(std::string_view file_path,
                                 AudioDataFormat& format,
                                 size_t& total_frames) const {}

// 创建解码器实例接口
std::unique_ptr<IDecoderInstance> FFmpegDecoderFactory::create_instance(
    std::string_view file_path) const {}

}  // namespace ice
