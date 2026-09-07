#ifndef ICE_CACHYDECODER_HPP
#define ICE_CACHYDECODER_HPP

#include <future>
#include <ice/manage/AudioFormat.hpp>
#include <ice/manage/dec/IDecoder.hpp>
#include <ice/manage/dec/IDecoderFactory.hpp>
#include <ice/thread/ThreadPool.hpp>
#include <optional>
#include <string_view>
#include <vector>

namespace ice
{
/// @brief 后台整文件解码，首次读取后保留不可变 PCM 缓存。
/// @warning 首次访问可等待 future；必须在进入实时播放前完成准备。
class CachyDecoder : public IDecoder
{
    using ChannelData = std::vector<float>;

public:
    /// @brief 将整文件解码任务提交到调用方提供的线程池。
    /// 返回时任务可能尚未完成，工厂和路径由任务持有。
    [[nodiscard]] static std::unique_ptr<CachyDecoder> create(
        std::string_view path, const ice::AudioDataFormat& target_format,
        ThreadPool& thread_pool, std::shared_ptr<IDecoderFactory> factory);

    /// @brief 获取实际缓存长度，不依赖媒体容器估算的总帧数。
    /// @warning 与 decode 一样，首次调用可能阻塞等待解码任务。
    size_t num_frames() const override
    {
        // 以解码后的首声道长度为准，容器中的预估时长可能不精确。
        const auto& data = get_data();
        // 确定下来的数据获取帧总数
        return data.pcm_data.empty() ? 0 : data.pcm_data[0].size();
    }
    /// @brief 复制缓存切片，返回每声道实际写入帧数。
    /// 多余声道及未写入的尾部不由此接口清零。
    size_t decode(float** buffer, uint16_t num_channels, size_t start_frame,
                  size_t frame_count) override;
    /// @brief 向容器追加只读切片；缓存对象必须活过所有借用视图。
    size_t origin(std::vector<std::span<const float>>& origin_data,
                  size_t start_frame, size_t frame_count) override;

private:
    // 包含解码结果的结构体
    struct DecodedData {
        AudioDataFormat                 format;
        std::vector<std::vector<float>> pcm_data;
    };

    // 私有构造函数,接收一个 future
    explicit CachyDecoder(std::future<DecodedData> future_data);

    /// @brief 使用一次性初始化发布结果，失败后缓存为空数据且不自动重试。
    const DecodedData& get_data() const;

    // future 持有后台解码任务的结果
    mutable std::future<DecodedData> future_data_;

    /// @brief 缓存一次性取得的解码结果，与进程退出回调无关。
    mutable std::optional<DecodedData> data_cache_;

    /// @brief 串行化首次结果提取，不等于整个初始化过程无等待。
    /// 后续只读借用依赖缓存发布后不再改变。
    mutable std::once_flag data_ready_flag_;
};

}  // namespace ice

#endif  // ICE_CACHYDECODER_HPP
