#pragma once

#include <ice/manage/AudioFormat.hpp>
#include <ice/manage/dec/IDecoder.hpp>

#include <cstddef>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace ice
{
/// @brief 后台任务执行器，公开接口仅借用其引用。
class ThreadPool;
/// @brief 解码实例工厂，公开接口仅传递共享所有权。
class IDecoderFactory;

/// @brief 后台整文件解码，首次读取后保留不可变 PCM 缓存。
/// @warning 首次访问可等待 future；必须在进入实时播放前完成准备。
class CachyDecoder : public IDecoder
{
    /// @brief 单个声道连续样本存储的类型别名。
    using ChannelData = std::vector<float>;

public:
    /// @brief 将整文件解码任务提交到调用方提供的线程池。
    /// 返回时任务可能尚未完成，工厂和路径由任务持有。
    /// @param path 非空且不含空字符的媒体路径，提交前复制，不借用调用方字符串。
    /// @param target_format
    /// 非零目标格式；后端须匹配采样率和声道数，允许单声道复制扩展。
    /// @param thread_pool 接收任务的后台线程池，停止时创建失败。
    /// @param factory 解码工厂，任务共享持有；空值或已知加载失败产生空缓存。
    /// @return 路径、目标格式无效或提交被拒绝时返回空指针；非空不代表解码成功。
    /// @warning 低频创建涉及分配与队列锁；历史提交/构造失败仍可能抛出异常。
    [[nodiscard]] static std::unique_ptr<CachyDecoder> create(
        std::string_view path, const ice::AudioDataFormat& target_format,
        ThreadPool& thread_pool, std::shared_ptr<IDecoderFactory> factory);

    /// @brief 获取实际缓存长度，不依赖媒体容器估算的总帧数。
    /// @warning 与 decode 一样，首次调用可能阻塞等待解码任务。
    size_t num_frames() const override
    {
        // 以解码后的首声道长度为准，容器中的预估时长可能不精确。
        const auto& data = get_data();
        return data.pcm_data.empty() ? 0 : data.pcm_data[0].size();
    }
    /// @brief 复制缓存切片，返回本次可用切片的帧数。
    /// 多余声道及未写入的尾部不由此接口清零。
    /// @param buffer 可写声道指针表，需覆盖 num_channels 个有效声道。
    /// @param num_channels 请求目标声道数；零声道或零帧立即返回零且不等待缓存。
    /// @param start_frame 每声道缓存起始帧，不是交错样本偏移。
    /// @param frame_count 目标容量允许的请求帧数，复制长度截断至缓存末尾。
    /// @pre 非空目标与内部缓存不重叠，各目标声道有足够容量。
    /// @warning 音频逐块读取链路：必须预先完成缓存加载，首次调用可能等待。
    /// 预备后只读缓存和复制，禁止在此追加文件操作或动态扩容。
    size_t decode(float** buffer, uint16_t num_channels, size_t start_frame,
                  size_t frame_count) override;
    /// @brief 向容器追加只读切片；缓存对象必须活过所有借用视图。
    /// @param origin_data 接收每个缓存声道视图的容器，不自动清理已有元素。
    /// @param start_frame 每声道切片起始帧。
    /// @param frame_count 请求帧数，按缓存剩余长度截断；零帧不等待后台任务。
    /// @return 每个新视图的帧数；零表示本次未追加视图，不表示容器为空。
    /// @warning 首次访问可能等待，追加可能分配；不能当作实时安全的零拷贝接口。
    size_t origin(std::vector<std::span<const float>>& origin_data,
                  size_t start_frame, size_t frame_count) override;

private:
    /// @brief 一次性发布的整文件解码结果，各声道保持相同帧数。
    struct DecodedData {
        /// @brief 解码实例报告的格式，必要时只调整单声道复制后的声道数。
        AudioDataFormat format;
        /// @brief 各声道独立连续存储，发布后不再改变容量或内容。
        std::vector<std::vector<float>> pcm_data;
    };

    /// @brief 只允许工厂生成的构造凭据，供 make_unique 调用公开构造函数。
    class ConstructionKey
    {
        friend class CachyDecoder;
        /// @brief 私有且非聚合的默认构造阻止外部用空花括号绕过工厂。
        ConstructionKey() {}

    public:
        /// @brief 允许标准库转发已有凭据，不允许无凭据创建实例。
        ConstructionKey(const ConstructionKey&) = default;
    };

public:
    /// @brief 接管唯一的异步结果提取句柄，不等待任务完成。
    /// @param key 工厂持有的构造凭据，标准库仅负责转发。
    /// @param future_data 后台整文件解码任务的结果。
    explicit CachyDecoder(ConstructionKey          key,
                          std::future<DecodedData> future_data);

private:
    /// @brief 使用一次性初始化发布结果，失败后缓存为空数据且不自动重试。
    /// @return 由解码器拥有的稳定缓存引用，不可跨越解码器析构。
    /// @warning 首次调用包含 call_once 与 future.get 等待；后续调用也经过
    /// call_once。 历史处理仅捕获
    /// std::exception，不能据此承诺所有异常都折叠为空数据。
    const DecodedData& get_data() const;

    /// @brief 由 call_once 内部唯一消费的后台结果句柄；get 后不再有效。
    mutable std::future<DecodedData> m_futureData;

    /// @brief 缓存一次性取得的解码结果，与进程退出回调无关。
    mutable std::optional<DecodedData> m_dataCache;

    /// @brief 串行化首次结果提取，不等于整个初始化过程无等待。
    /// 后续只读借用依赖缓存发布后不再改变。
    mutable std::once_flag m_dataReadyFlag;
};

}  // namespace ice
