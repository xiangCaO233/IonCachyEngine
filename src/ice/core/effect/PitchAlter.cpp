#include <rubberband/RubberBandStretcher.h>

#include <cmath>
#include <ice/core/effect/PitchAlter.hpp>

namespace ice {
// 应用变调器
void PitchAlter::apply_effect(AudioBuffer& output, const AudioBuffer& input) {
    // 惰性初始化
    if (!stretcher) {
        stretcher = std::make_unique<RStretcher>(input.afmt);
    }
    // 设置 RubberBand 的参数
    // 音高倍率 = 2 ^ (半音数 / 12)
    const double pitch_ratio = std::pow(2.0, pitch_semitones.load() / 12.0);
    stretcher->set_pitch_ratio(pitch_ratio);
    // 实时处理
    stretcher->process(output, input);
}
}  // namespace ice
