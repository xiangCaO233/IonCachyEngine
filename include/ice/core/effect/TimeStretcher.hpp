#ifndef ICE_TIMESTRETCHER_HPP
#define ICE_TIMESTRETCHER_HPP

#include <atomic>
#include <ice/core/effect/rubberband/RStretcher.hpp>
#include <memory>

#include "ice/core/effect/IEffectNode.hpp"

namespace ice
{
class TimeStretcher : public IEffectNode
{
public:
    void process(AudioBuffer& buffer) override;

    /**
     * @brief 设置期望的播放速度倍率。
     *        1.0为原速，<1.0为慢放，>1.0为快放。
     * @param desired_ratio 期望的播放速度倍率。
     */
    inline void set_playback_ratio(double desired_ratio)
    {
        // 在这里可以对 desired_ratio 进行范围检查
        if ( desired_ratio >= 0.05 && desired_ratio <= 10.0 )
            {
                desired_playback_ratio.store(desired_ratio);
            }
    }

    /**
     * @brief 获取最近一次 process() 调用中实际生效的播放速度倍率。
     *        由于帧的离散性，这个值可能与 set_playback_ratio()
     * 设置的值有微小差异。
     * @return float 实际生效的播放速度倍率。
     */
    [[nodiscard]] inline double get_actual_playback_ratio() const
    {
        return actual_playback_ratio.load();
    }

protected:
    // 应用效果实现
    void apply_effect(AudioBuffer& output, const AudioBuffer& input) override;

private:
    // 期望值
    std::atomic<double> desired_playback_ratio{ 1.0 };
    // 实际值
    std::atomic<double> actual_playback_ratio{ 1.0 };
    // 实际拉伸倍率
    std::atomic<double> actual_stretch_ratio{ 1.0 };

    // 拉伸器
    std::unique_ptr<RStretcher> stretcher;
};

}  // namespace ice

#endif  // ICE_TIMESTRETCHER_HPP
