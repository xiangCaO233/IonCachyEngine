#pragma once

#include "ice/manage/dec/IDecoder.hpp"
#include <memory>
#include <string_view>

namespace ice
{
class ThreadPool;
class IDecoderFactory;

/// @brief 后台按块预读，PCM 内存不随文件长度增长。
/// 缓存缺页时输出静音并请求对应区间，下一次读取自动使用就绪数据。
/// origin 不提供临时缓存视图，上层分析应单独选择完全缓存策略。
class StreamingDecoder final : public IDecoder
{
public:
    /// @brief 创建实例并同步准备首块，随后启动专用预读线程。
    /// @warning 低频资源操作，包含文件访问、分配和首块解码。
    /// thread_pool 为策略接口兼容参数，长期预读不占用其工作线程。
    static std::unique_ptr<StreamingDecoder> create(
        std::string_view path, const AudioDataFormat& target_format,
        ThreadPool& thread_pool, std::shared_ptr<IDecoderFactory> factory);
    /// @brief 建立尚未启动的空状态，实际使用应通过 create。
    StreamingDecoder();
    /// @brief 唤醒并停止预读线程，然后释放全部缓存。
    /// @warning 仅在控制侧销毁；join 可能等待当前文件读取完成。
    ~StreamingDecoder() override;
    /// @brief 返回目标采样率下的长度估计，遇到文件尾后修正为实际长度。
    /// @warning 逐块查询仅 relaxed 读取工作线程更新的独立长度，不执行 IO。
    size_t num_frames() const override;
    /// @brief 复制就绪块，缺页或竞争时补静音，返回时间线上有效的帧数。
    /// 返回零只表示越过已知末尾或请求无效，不把缺页误报为播放结束。
    /// @warning 每音频块调用，只尝试加锁一次，不等待、不分配、不执行 IO。
    /// 预读通知由读取侧发布，工作线程消费；原子计数用于避免丢失唤醒。
    size_t decode(float** buffer, uint16_t num_channels, size_t start_frame,
                  size_t frame_count) override;
    /// @brief 不提供可长期借用的 PCM，返回零且保持容器不变。
    /// 预读块会被回收，不能把其地址暴露给波形或离线 DSP。
    size_t origin(std::vector<std::span<const float>>& origin_data,
                  size_t start_frame, size_t frame_count) override;

private:
    /// @brief 隐藏缓存、同步和解码器实例，析构处拥有完整类型。
    struct State;
    /// @brief 独占状态；读取期间由外部保证解码器存活。
    std::unique_ptr<State> m_state;
};
}  // namespace ice
