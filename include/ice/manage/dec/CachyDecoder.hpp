#ifndef ICE_CACHYDECODER_HPP
#define ICE_CACHYDECODER_HPP

#include <future>
#include <ice/thread/ThreadPool.hpp>
#include <optional>
#include <string_view>
#include <vector>

#include "ice/manage/dec/IDecoder.hpp"
#include "ice/manage/dec/IDecoderFactory.hpp"

namespace ice {
class CachyDecoder : public IDecoder {
    using ChannelData = std::vector<float>;

   public:
    // 工厂方法
    [[nodiscard]] static std::unique_ptr<CachyDecoder> create(
        std::string_view path, ThreadPool& thread_pool,
        std::shared_ptr<IDecoderFactory> factory);

    size_t num_frames() const override {
        const auto& data = get_data();
        // 确定下来的数据获取帧总数
        return data.pcm_data.empty() ? 0 : data.pcm_data[0].size();
    }
    // 解码数据到缓冲区的接口
    double decode(float** buffer, uint16_t num_channels, double start_frame,
                  double frame_count) override;
    // 获取原始数据接口
    double origin(std::vector<std::span<const float>>& origin_data,
                  double start_frame, double frame_count) override;

   private:
    // 包含解码结果的结构体
    struct DecodedData {
        AudioDataFormat format;
        std::vector<std::vector<float>> pcm_data;
    };

    // 私有构造函数,接收一个 future
    explicit CachyDecoder(std::future<DecodedData> future_data);

    // 线程安全地获取解码结果
    const DecodedData& get_data() const;

    // future 持有后台解码任务的结果
    mutable std::future<DecodedData> future_data_;

    // atexit_handler 持有最终解码的结果
    mutable std::optional<DecodedData> data_cache_;

    // 确保 get_data() 的初始化只发生一次
    mutable std::once_flag data_ready_flag_;
};

}  // namespace ice

#endif  // ICE_CACHYDECODER_HPP
