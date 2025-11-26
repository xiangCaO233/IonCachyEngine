#ifndef ICE_GRAPHICEQUALIZER_HPP
#define ICE_GRAPHICEQUALIZER_HPP

#include <ice/core/effect/IEffectNode.hpp>
#include <ice/core/effect/filter/BiquadFilter.hpp>
#include <ice/manage/AudioBuffer.hpp>
#include <numbers>
#include <vector>

namespace ice
{
// EQ频段设置
struct EQBandOptions
{
    double center_freq_hz;
    // 默认Q值
    double q_factor{ std::numbers::sqrt2 };
    // 默认增益(dB)
    double gain_db{ 0.0 };
};

// 均衡器
class GraphicEqualizer : public IEffectNode
{
public:
    // 构造时传入一个频段列表
    explicit GraphicEqualizer(const std::vector<double>& center_frequencies);

    // 控制接口
    // 按索引设置频段增益（通过浮点倍率）
    void set_band_gain_ratio(size_t band_index, float ratio);

    // 按索引设置频段增益（通过分贝）
    void set_band_gain_db(size_t band_index, float db);

    /**
     * @brief 设置指定频段的Q因子（品质因数）
     *
     * Q因子决定了均衡器频段的带宽，即影响范围的宽度
     * - 较高的Q值 (如 5.0)
     * 会产生一个很窄的、尖锐的滤波器，只影响中心频率附近很小的范围
     * - 较低的Q值 (如 0.7) 会产生一个很宽的、平缓的滤波器，影响范围更广
     *
     * @param band_index 要修改的频段的索引 (从0开始)。
     * @param q          新的Q因子值，必须为正数。
     */
    void set_band_q_factor(size_t band_index, float q)
    {
        // 确保索引有效.且Q值为正数
        if ( band_index < bands.size() && q > 0.0f )
            {
                bands[band_index].q_factor = q;
                // 标记滤波器需要更新
                needs_update = true;
            }
    }

protected:
    // 应用均衡器
    void apply_effect(AudioBuffer& output, const AudioBuffer& input) override;

private:
    // 当参数改变时,重新计算滤波器系数
    void update_filters();

    // 存储用户设置的每个频段的信息
    std::vector<EQBandOptions> bands;

    // 滤波器链
    // 每个声道都需要独立的一套滤波器
    std::vector<std::vector<BiquadFilter>> filter_chains;

    // 采样率
    double sample_rate{ 0.0 };

    // 滤波器系数是否需要重新计算
    bool needs_update{ true };
};

}  // namespace ice

#endif  // ICE_GRAPHICEQUALIZER_HPP
