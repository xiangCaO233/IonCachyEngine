#ifndef ICE_PITCHALTER_HPP
#define ICE_PITCHALTER_HPP

#include <atomic>
#include <ice/core/effect/rubberband/RStretcher.hpp>

#include "ice/core/effect/IEffectNode.hpp"

namespace ice {
class PitchAlter : public IEffectNode {
   public:
    /**
     * @brief 设置音高变化,以半音（semitones）为单位
     * e.g., +12.0f 是升高一个八度, -12.0f 是降低一个八度
     * @param semitones 音高变化
     */
    inline void set_pitch_shift(double semitones) {
        pitch_semitones.store(semitones);
    }

   protected:
    // 应用变调器
    void apply_effect(AudioBuffer& output, const AudioBuffer& input) override;

   private:
    // 音高变化
    std::atomic<double> pitch_semitones;
    // 拉伸器
    std::unique_ptr<RStretcher> stretcher;
};
}  // namespace ice

#endif  // ICE_PITCHALTER_HPP
