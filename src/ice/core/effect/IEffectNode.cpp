#include <ice/core/effect/IEffectNode.hpp>

namespace ice {
// 构造IEffectNode
IEffectNode::IEffectNode(std::shared_ptr<IAudioNode> input)
    : inputNode(input) {}

void IEffectNode::process(AudioBuffer& buffer, uint32_t frame_count) {
    // 从上游节点拉取数据到我们自己的临时输入缓冲区
    // (需要确保 m_inputBuffer 大小足够)
    inputNode->process(inputBuffer, frame_count);

    // 对输入缓冲区的数据应用效果，并将结果放入最终的输出缓冲区
    apply_effect(buffer, inputBuffer, frame_count);
}
}  // namespace ice
