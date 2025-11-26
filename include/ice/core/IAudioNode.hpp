#ifndef ICE_IAUDIONODE_HPP
#define ICE_IAUDIONODE_HPP

#include "ice/manage/AudioBuffer.hpp"

namespace ice
{

// 节点接口
class IAudioNode
{
public:
    // 构造IAudioNode
    IAudioNode() = default;
    // 析构IAudioNode
    virtual ~IAudioNode() = default;

    // 节点的通用处理接口
    virtual void process(AudioBuffer& buffer) = 0;
};

}  // namespace ice

#endif  // ICE_IAUDIONODE_HPP
