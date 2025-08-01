#include <rubberband/RubberBandStretcher.h>

#include <ice/core/effect/PitchAlter.hpp>

namespace ice {
// 应用变调器
void PitchAlter::apply_effect(AudioBuffer& output, const AudioBuffer& input) {
    // 惰性初始化
    if (!stretcher) {
        stretcher = std::make_unique<RStretcher>(input.afmt);
    }

    stretcher->set_pitch_ratio(pitch_scale.load());

    // 实时处理
    stretcher->process(output, input);
}
}  // namespace ice
