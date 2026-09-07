#ifndef ICE_PLAYCALLBACK_HPP
#define ICE_PLAYCALLBACK_HPP

#include <chrono>
#include <cstddef>

namespace ice
{
/// @brief 播放状态通知接口，不移交播放器或缓冲所有权。
/// 回调执行线程由播放器决定，UI 更新需要上层自行转发。
/// 回调实现自行管理接收者生命周期，接口不持有播放节点所有权。
class PlayCallBack
{
public:
    // 构造PlayCallBack
    PlayCallBack() = default;
    // 析构PlayCallBack
    virtual ~PlayCallBack() = default;

    // 播放完成完整一遍回调(传入是否循环)
    virtual void play_done(bool loop) const = 0;

    /// @brief 以采样帧为单位报告进度，不是累计字节数。
    virtual void frameplaypos_updated(size_t frame_pos) = 0;

    /// @brief 报告纳秒精度的时间位置，接收方不应假定它是系统时钟时间。
    virtual void timeplaypos_updated(std::chrono::nanoseconds time_pos) = 0;
};
}  // namespace ice

#endif  // ICE_PLAYCALLBACK_HPP
