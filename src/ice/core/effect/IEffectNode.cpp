#include <ice/core/effect/IEffectNode.hpp>

namespace ice {

void IEffectNode::process(AudioBuffer& buffer) {
    if (inputNode) {
        // (需要确保 m_inputBuffer 大小足够)
        inputBuffer.resize(buffer.afmt, buffer.num_frames());
        inputBuffer.clear();

        // 从上游节点拉取数据到我们自己的临时输入缓冲区
        inputNode->process(inputBuffer);

        // 对输入缓冲区的数据应用效果，并将结果放入最终的输出缓冲区
        apply_effect(buffer, inputBuffer);
    }
}
}  // namespace ice
