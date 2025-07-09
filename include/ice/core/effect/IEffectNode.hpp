#ifndef ICE_IEFFECTNODE_HPP
#define ICE_IEFFECTNODE_HPP

#include <memory>

#include "ice/core/IAudioNode.hpp"
#include "ice/manage/AudioBuffer.hpp"

namespace ice {
class IEffectNode : public IAudioNode {
   public:
    // 构造IEffectNode
    explicit IEffectNode(std::shared_ptr<IAudioNode> input);

    // 析构IEffectNode
    ~IEffectNode() override = default;

    void process(AudioBuffer& buffer, uint32_t frame_count) override;

   protected:
    // 上游节点
    std::shared_ptr<ice::IAudioNode> inputNode;

    // 上游输入缓冲
    AudioBuffer inputBuffer;

    // 应用效果的纯虚接口
    virtual void apply_effect(AudioBuffer& output, const AudioBuffer& input,
                              uint32_t frameCount) = 0;
};
}  // namespace ice

#endif  // ICE_IEFFECTNODE_HPP
