#ifndef ICE_COMPRESSER_HPP
#define ICE_COMPRESSER_HPP

#include "ice/core/effect/IEffectNode.hpp"
namespace ice {
class Compresser : public IEffectNode {
   protected:
    // 应用压缩器
    void apply_effect(AudioBuffer& output, const AudioBuffer& input) override;
};
}  // namespace ice

#endif  // ICE_COMPRESSER_HPP
