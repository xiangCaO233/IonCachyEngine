#ifndef ICE_CLIPPER_HPP
#define ICE_CLIPPER_HPP

#include <ice/core/effect/IEffectNode.hpp>

namespace ice {
class Clipper : public IEffectNode {
   public:
    // 构造Clipper
    Clipper();
    // 析构Clipper
    ~Clipper() override = default;

   protected:
    // 应用效果
    void apply_effect(AudioBuffer& output, const AudioBuffer& input) override;
};

}  // namespace ice

#endif  // ICE_CLIPPER_HPP
