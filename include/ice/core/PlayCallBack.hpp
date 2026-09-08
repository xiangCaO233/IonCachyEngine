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
    /// @brief 构造通知接口，不自动注册到播放节点。
    PlayCallBack() = default;
    /// @brief 析构通知接口，不负责从外部播放节点移除注册。
    virtual ~PlayCallBack() = default;

    /// @brief 通知 SourceNode
    /// 已到达一次输入末尾，不代表设备或后端尾音已经播放完。
    /// @param loop 是否进入下一轮循环；为真时节点游标已回到零。
    /// @warning 音频处理内同步调用，不得分配、阻塞或修改当前节点回调集合。
    virtual void play_done(bool loop) const = 0;

    /// @brief 以采样帧为单位报告进度，不是累计字节数。
    /// @param frame_pos SourceNode
    /// 下一次读取的游标，循环边界可归零，不保证单调递增。
    /// @warning 正常处理块内同步通知，不能将 UI 更新或文件写入直接放入回调。
    virtual void frameplaypos_updated(size_t frame_pos) = 0;

    /// @brief 报告纳秒精度的时间位置，接收方不应假定它是系统时钟时间。
    /// @param time_pos
    /// 由同次通知的帧游标按内部采样率换算并截取为纳秒的媒体位置。
    /// 纳秒是表示单位，不承诺采样定位或设备播放时间具有纳秒精度。
    /// @warning 与帧位置通知同在音频处理线程执行，禁止阻塞等待其他线程确认。
    virtual void timeplaypos_updated(std::chrono::nanoseconds time_pos) = 0;
};
}  // namespace ice

#endif  // ICE_PLAYCALLBACK_HPP
