#include <ice/core/effect/Clipper.hpp>

#include "ice/core/effect/IEffectNode.hpp"

namespace ice {
// 构造Clipper
Clipper::Clipper(std::shared_ptr<IAudioNode> input) : IEffectNode(input) {}

// 应用效果
void Clipper::apply_effect(AudioBuffer& output, const AudioBuffer& input) {}

}  // namespace ice
