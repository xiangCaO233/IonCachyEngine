#ifndef ICE_STREAMINGDECODER_HPP
#define ICE_STREAMINGDECODER_HPP

#include <ice/manage/dec/IDecoderFactory.hpp>
#include <ice/thread/ThreadPool.hpp>
#include <memory>

#include "ice/manage/dec/IDecoder.hpp"

namespace ice {
// TODO(xiang 2025-07-11): 实现流式解码器
class StreamingDecoder : public IDecoder {
   public:
    // 工厂方法
    [[nodiscard]] static std::unique_ptr<StreamingDecoder> create(
        std::string_view path, ThreadPool& thread_pool,
        std::shared_ptr<IDecoderFactory> factory);

    // 解码数据到缓冲区的接口
    double decode(float** buffer, uint16_t num_channels, size_t start_frame,
                  size_t frame_count) override;

    // 获取原始数据接口
    double origin(std::vector<std::span<const float>>& origin_data,
                  size_t start_frame, size_t frame_count) override;

    size_t num_frames() const override { return 0; }

   private:
    // 构造StreamingDecoder
    explicit StreamingDecoder(std::string_view path, ThreadPool& thread_pool,
                              std::shared_ptr<IDecoderFactory> factory);
};

}  // namespace ice

#endif  // ICE_STREAMINGDECODER_HPP
