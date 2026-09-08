#ifndef ICE_IAUDIONODE_HPP
#define ICE_IAUDIONODE_HPP

#include "ice/manage/AudioBuffer.hpp"

namespace ice
{

/// @brief 音频图节点的最小接口，调用方拥有传入缓冲。
/// @details
/// 接口不提供线程同步或图调度；同一实例能否并发处理由具体节点契约决定。
/// 不负责保活上游节点，也不保证派生实现自动满足实时约束。
class IAudioNode
{
public:
    /// @brief 构造无状态接口部分，不准备音频设备或处理缓冲。
    IAudioNode() = default;
    /// @brief 允许通过接口销毁派生节点，不负责停止正在执行的音频回调。
    /// @warning
    /// 销毁前由调用方确保处理已经结束；派生析构可能回收缓冲或等待线程。
    virtual ~IAudioNode() = default;

    /// @brief 在既定格式与活动帧区间内处理输出。
    /// @param buffer 调用方提供的可写缓冲，借用仅覆盖本次同步调用。
    /// @details
    /// 返回类型没有短读计数或错误码，失败及未填满区间的约定由具体节点说明。
    /// 调用方不能仅根据正常返回就认定整块已经写入有效音频。
    /// @warning 音频周期热路径；不得新增分配、锁或阻塞等待。
    /// 节点应遵守上层预备的缓冲格式与帧数，不能擅自扩大活动区间。
    virtual void process(AudioBuffer& buffer) = 0;
};

}  // namespace ice

#endif  // ICE_IAUDIONODE_HPP
