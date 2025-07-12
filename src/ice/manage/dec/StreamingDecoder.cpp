#include "ice/manage/dec/StreamingDecoder.hpp"
namespace ice {
// TODO(xiang 2025-07-12):实现流式解码器接口
// 工厂方法
[[nodiscard]] std::unique_ptr<StreamingDecoder> StreamingDecoder::create(
    std::string_view path, const IDecoderFactory& factory) {}
StreamingDecoder::StreamingDecoder(std::string_view path,
                                   const IDecoderFactory& factory) {}
// 解码数据到缓冲区的接口
size_t StreamingDecoder::decode(float** buffer, uint16_t num_channels,
                                size_t start_frame, size_t frame_count) {
    // TODO(xiang 2025-07-11): 实现流式解码
    return 0;
}
}  // namespace ice
