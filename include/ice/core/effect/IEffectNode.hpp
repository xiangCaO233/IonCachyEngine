#ifndef ICE_IEFFECTNODE_HPP
#define ICE_IEFFECTNODE_HPP

#include <memory>

#include "ice/core/IAudioNode.hpp"
#include "ice/manage/AudioBuffer.hpp"

namespace ice
{
class IEffectNode : public IAudioNode
{
public:
    // 构造IEffectNode
    IEffectNode() = default;

    // 析构IEffectNode
    ~IEffectNode() override = default;

    void process(AudioBuffer& buffer) override;

    // 指定输入节点
    inline void set_inputnode(std::shared_ptr<IAudioNode> input)
    {
        inputNode = input;
    }

protected:
    inline std::shared_ptr<ice::IAudioNode> get_inputnode() const
    {
        return inputNode;
    }
    inline AudioBuffer& get_inputbuffer() { return inputBuffer; }

    // 应用效果的纯虚接口
    virtual void apply_effect(AudioBuffer&       output,
                              const AudioBuffer& input) = 0;

private:
    // 上游节点
    std::shared_ptr<ice::IAudioNode> inputNode{ nullptr };

    // 上游输入缓冲
    AudioBuffer inputBuffer;
};
}  // namespace ice

#endif  // ICE_IEFFECTNODE_HPP
