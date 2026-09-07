#ifndef ICE_PITCHALTER_HPP
#define ICE_PITCHALTER_HPP

#include <atomic>
#include <cmath>
#include <ice/core/effect/IEffectNode.hpp>
#include <ice/core/effect/rubberband/RStretcher.hpp>

namespace ice
{
/// @brief 历史单独变调节点，保持时间倍率不变。
class PitchAlter : public IEffectNode
{
public:
    /**
     * @brief 设置音高变化,直接设置倍率
     * @param scale 音高倍率
     */
    inline void set_pitch_scale(double scale)
    {
        // 倍率必须为正，半音换算使用以 2 为底的对数。
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
        // 每十二个半音对应倍率翻倍；两次原子写入不构成一致快照。
        pitch_semitones.store(semitones);
        pitch_scale.store(std::pow(2.0, pitch_semitones.load() / 12.0));
    }

    // 获取音高变化值
    inline double semitones() const { return pitch_semitones.load(); }

    // 获取音高倍率变化值
    inline double scale() const { return pitch_scale.load(); }

    /// @brief 拉取上游并按音高倍率处理音频。
    /// @warning 每块路径仍存在容量调整和共享指针复制，需要后续实时化。
    void process(AudioBuffer& buffer) override;

protected:
    /// @brief 惰性建立并调用后端变调器。
    /// @warning 首次处理可分配和预热，不能在严格实时回调中首次触发。
    void apply_effect(AudioBuffer& output, const AudioBuffer& input) override;

private:
    /// @brief 控制侧写入的半音表示，音频处理实际消费倍率字段。
    std::atomic<double> pitch_semitones{ 0. };
    /// @brief 控制侧写、音频线程按块读的倍率，不与半音字段形成事务。
    std::atomic<double> pitch_scale{ 1. };
    /// @brief 首次效果处理时建立的后端，生命周期由节点独占。
    std::unique_ptr<RStretcher> stretcher;
};
}  // namespace ice

#endif  // ICE_PITCHALTER_HPP
