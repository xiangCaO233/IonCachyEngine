#ifndef ICE_STREAMINGDECODER_HPP
#define ICE_STREAMINGDECODER_HPP

#include <ice/manage/dec/IDecoderFactory.hpp>
#include <memory>

#include "ice/manage/dec/IDecoder.hpp"

namespace ice {
// TODO(xiang 2025-07-11): 实现流式解码器
class StreamingDecoder : public IDecoder {
   public:
    // 工厂方法
    [[nodiscard]] static std::unique_ptr<StreamingDecoder> create(
        std::string_view path, const IDecoderFactory& factory);

    // 解码数据到缓冲区的接口
    size_t decode(float** buffer, uint16_t num_channels, size_t start_frame,
                  size_t frame_count) override;

    size_t num_frames() const override { return 0; }

   private:
    // 构造StreamingDecoder
    explicit StreamingDecoder(std::string_view path,
                              const IDecoderFactory& factory);
};

}  // namespace ice

#endif  // ICE_STREAMINGDECODER_HPP
