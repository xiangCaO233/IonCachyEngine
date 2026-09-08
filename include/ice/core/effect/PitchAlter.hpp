#ifndef ICE_PITCHALTER_HPP
#define ICE_PITCHALTER_HPP

#include <atomic>
#include <cmath>
#include <ice/core/effect/IEffectNode.hpp>
#include <ice/core/effect/rubberband/RStretcher.hpp>

namespace ice
{
/// @brief 历史单独变调节点，保持时间倍率不变。
/// @details 按设备块等量请求输入，不按音高倍率缩放上游请求帧数。
/// 同一节点的缓冲与后端不能由多个音频线程同时处理。
class PitchAlter : public IEffectNode
{
public:
    /**
     * @brief 设置音高变化,直接设置倍率
     * @param scale 音高倍率
     * @pre 倍率必须有限且大于零，本接口不验证对数定义域。
     * @warning 控制侧默认顺序一致地写两字段，不是成对原子发布；多个 setter
     * 应串行。
     */
    inline void set_pitch_scale(double scale)
    {
        // 倍率必须为正，半音换算使用以 2 为底的对数。
        pitch_scale.store(scale);
        pitch_semitones.store(12.0 * std::log2(scale));
    }

    /**
     * @brief 设置音高变化,以半音（semitones）为单位
     * 例如 +12.0f 是升高一个八度，-12.0f 是降低一个八度。
     * @param semitones 音高变化
     * @pre 半音必须有限，换算后的倍率须保持有限且大于零；未做溢出或下溢保护。
     */
    inline void set_pitch_shift(double semitones)
    {
        // 每十二个半音对应倍率翻倍；两次原子写入不构成一致快照。
        pitch_semitones.store(semitones);
        pitch_scale.store(std::pow(2.0, pitch_semitones.load() / 12.0));
    }

    /// @brief 查询最近保存的半音表示，不代表后端已经消费该参数。
    /// @return 独立原子读取的半音值，不能与 scale 组合成一致快照。
    inline double semitones() const { return pitch_semitones.load(); }

    /// @brief 查询最近保存的倍率表示，不代表后端已经应用它。
    inline double scale() const { return pitch_scale.load(); }

    /// @brief 拉取上游并按音高倍率处理音频。
    /// @warning 每块路径仍存在容量调整和共享指针复制，需要后续实时化。
    /// @warning process 每块读取控制侧倍率，apply_effect
    /// 可能再次读取，当前默认顺序一致。 上游所有权复制沿用历史
    /// getter；运行期间本应保持上游稳定，尚未迁移为观察访问。
    void process(AudioBuffer& buffer) override;

protected:
    /// @brief 惰性建立并调用后端变调器。
    /// @warning 首次处理可分配和预热，不能在严格实时回调中首次触发。
    /// @warning 处理侧向后端写入控制侧倍率的原子快照，不与另一次 process
    /// 读取成事务。
    void apply_effect(AudioBuffer& output, const AudioBuffer& input) override;

private:
    /// @brief 控制侧写入的半音表示，音频处理实际消费倍率字段。
    /// @warning setter
    /// 与查询接口默认顺序一致地读写；只保护单字段，无法保护换算过程。
    std::atomic<double> pitch_semitones{ 0. };
    /// @brief 控制侧写、音频线程按块读的倍率，不与半音字段形成事务。
    /// @warning
    /// 每块可能读取两次，默认顺序一致；原子不保护后端和复用缓冲的并发访问。
    std::atomic<double> pitch_scale{ 1. };
    /// @brief 首次效果处理时建立的后端，生命周期由节点独占。
    std::unique_ptr<RStretcher> stretcher;
};
}  // namespace ice

#endif  // ICE_PITCHALTER_HPP
