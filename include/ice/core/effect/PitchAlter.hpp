#ifndef ICE_PITCHALTER_HPP
#define ICE_PITCHALTER_HPP

#include <atomic>
#include <memory>

#include "ice/core/effect/IEffectNode.hpp"

namespace ice {
class PAlterImp;
class PitchAlter : public IEffectNode {
   public:
    PitchAlter();
    ~PitchAlter() override;

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
    // 变调器实现
    std::unique_ptr<PAlterImp> palterimpl;
};
}  // namespace ice

#endif  // ICE_PITCHALTER_HPP
