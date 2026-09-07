#ifndef ICE_IAUDIONODE_HPP
#define ICE_IAUDIONODE_HPP

#include "ice/manage/AudioBuffer.hpp"

namespace ice
{

/// @brief 音频图节点的最小接口，调用方拥有传入缓冲。
class IAudioNode
{
public:
    // 构造IAudioNode
    IAudioNode() = default;
    // 析构IAudioNode
    virtual ~IAudioNode() = default;

    /// @brief 在既定格式与活动帧区间内处理输出。
    /// @warning 音频周期热路径；不得新增分配、锁或阻塞等待。
    /// 节点应遵守上层预备的缓冲格式与帧数，不能擅自扩大活动区间。
    virtual void process(AudioBuffer& buffer) = 0;
};

}  // namespace ice

#endif  // ICE_IAUDIONODE_HPP
