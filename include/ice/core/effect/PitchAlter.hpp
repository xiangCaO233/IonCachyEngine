#ifndef ICE_PITCHALTER_HPP
#define ICE_PITCHALTER_HPP

#include <atomic>
#include <cmath>
#include <ice/core/effect/IEffectNode.hpp>
#include <ice/core/effect/rubberband/RStretcher.hpp>

namespace ice
{
class PitchAlter : public IEffectNode
{
public:
    /**
     * @brief 设置音高变化,直接设置倍率
     * @param scale 音高倍率
     */
    inline void set_pitch_scale(double scale)
    {
        pitch_scale.store(scale);
        pitch_semitones.store(12.0 * std::log2(scale));
    }

    /**
     * @brief 设置音高变化,以半音（semitones）为单位
     * e.g., +12.0f 是升高一个八度, -12.0f 是降低一个八度
     * @param semitones 音高变化
     */
    inline void set_pitch_shift(double semitones)
    {
        pitch_semitones.store(semitones);
        pitch_scale.store(std::pow(2.0, pitch_semitones.load() / 12.0));
    }

    // 获取音高变化值
    inline double semitones() const { return pitch_semitones.load(); }

    // 获取音高倍率变化值
    inline double scale() const { return pitch_scale.load(); }

    void process(AudioBuffer& buffer) override;

protected:
    // 应用变调器
    void apply_effect(AudioBuffer& output, const AudioBuffer& input) override;

private:
    // 音高变化
    std::atomic<double> pitch_semitones{ 0. };
    std::atomic<double> pitch_scale{ 1. };
    // 拉伸器
    std::unique_ptr<RStretcher> stretcher;
};
}  // namespace ice

#endif  // ICE_PITCHALTER_HPP
