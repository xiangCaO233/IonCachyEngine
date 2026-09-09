#pragma once

#include "ice/manage/dec/IDecoder.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace ice
{
class ThreadPool;
class IDecoderFactory;
struct AudioDataFormat;

/// @brief 后台按块预读，PCM 内存不随文件长度增长。
/// 缓存缺页时输出静音；取得缓存锁且队列有空间时请求对应区间。
/// 后续读取仅在页面已发布且成功取得锁时使用 PCM，不保证下一块必然就绪。
/// origin 不提供临时缓存视图，上层分析应单独选择完全缓存策略。
class StreamingDecoder final : public IDecoder
{
public:
    /// @brief 创建实例并同步准备首块，随后启动专用预读线程。
    /// @param path 非空且不含空字符的媒体路径，仅在工厂调用期间借用。
    /// @param target_format 非零目标格式，后端必须返回相同声道数与采样率。
    /// @param thread_pool 策略接口兼容参数，本实现不向该池提交任务。
    /// @param factory 同步创建后端实例的工厂，返回后由状态独占实例。
    /// @warning 低频资源操作，包含文件访问、分配和首块解码。
    /// thread_pool 为策略接口兼容参数，长期预读不占用其工作线程。
    /// @return 无效路径、空工厂、不匹配格式、零估算长度及初始定位失败返回空。
    /// 线程创建失败也返回空；首次短读可创建零长度对象，后端异常仍沿用历史通道。
    static std::unique_ptr<StreamingDecoder> create(
        std::string_view path, const AudioDataFormat& target_format,
        ThreadPool& thread_pool, std::shared_ptr<IDecoderFactory> factory);
    /// @brief 建立尚未启动的空状态，实际使用应通过 create。
    StreamingDecoder();
    /// @brief 唤醒并停止预读线程，然后释放全部缓存。
    /// @warning 仅在控制侧销毁；join 可能等待当前文件读取完成。
    ~StreamingDecoder() override;
    /// @brief 返回目标采样率下的长度估计，后端短读时按当前游标修正。
    /// @return 当前帧数快照；空对象返回零，不作为缓存页就绪证明。
    /// 不区分短读与解码失败；估算偏小时不保证继续请求估算末尾之外的页。
    /// @warning 逐块查询仅 relaxed 读取工作线程更新的独立长度，不执行 IO。
    size_t num_frames() const override;
    /// @brief 复制就绪块，缺页或竞争时补静音，返回时间线上有效的帧数。
    /// 返回零只表示越过已知末尾或请求无效，不把缺页误报为播放结束。
    /// @param buffer 非空可写声道指针表，不与内部缓存存储重叠。
    /// @param num_channels 输出表声道数，多余声道填充静音。
    /// @param start_frame 目标采样率下的绝对起始帧，不修改共享播放游标。
    /// @param frame_count 每声道请求容量，越过已知末尾的部分保持原样。
    /// @return 按长度快照裁剪的推进帧数，包含缺页时填充的静音。
    /// @pre 各输出平面可写且容量足够；输出表无效时在任何清零前返回零。
    /// @warning 每音频块调用，只尝试加锁一次，不等待、不分配、不执行 IO。
    /// 预读通知由读取侧发布，工作线程消费；原子计数用于避免丢失唤醒。
    size_t decode(float** buffer, uint16_t num_channels, size_t start_frame,
                  size_t frame_count) override;
    /// @brief 不提供可长期借用的 PCM，返回零且保持容器不变。
    /// 预读块会被回收，不能把其地址暴露给波形或离线 DSP。
    /// @param origin_data 保持不变的调用方视图容器。
    /// @param start_frame 接口兼容参数，不触发定位或预读。
    /// @param frame_count 接口兼容参数，不触发缓存分配。
    /// @return 始终为零，调用方应使用其他解码策略获取稳定视图。
    size_t origin(std::vector<std::span<const float>>& origin_data,
                  size_t start_frame, size_t frame_count) override;

private:
    /// @brief 隐藏缓存、同步和解码器实例，析构处拥有完整类型。
    struct State;
    /// @brief 独占状态；读取期间由外部保证解码器存活。
    std::unique_ptr<State> m_state;
};
}  // namespace ice
