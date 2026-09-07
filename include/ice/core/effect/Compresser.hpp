#ifndef ICE_COMPRESSER_HPP
#define ICE_COMPRESSER_HPP

#include <atomic>

#include "ice/config/config.hpp"
#include "ice/core/effect/IEffectNode.hpp"
namespace ice
{

/// @brief 各声道独立维护包络的动态范围压缩器。
/// @warning setter 会写非原子脏标记，参数更新必须与音频处理串行。
class Compressor : public IEffectNode
{
public:
    /// @brief 设置开始压缩的电平阈值，单位为 dB。
    inline void set_threshold_db(float db)
    {
        // 参数先更新，再请求下一次效果调用重算系数；两步不是原子事务。
        threshold_db.store(db);
        needs_update = true;
    }
    /// @brief 设置超阈值部分的压缩比；调用方保证数值有效且非零。
    inline void set_ratio(float vratio)
    {
        ratio.store(vratio);
        needs_update = true;
    }
    /// @brief 设置上升包络的时间参数，单位毫秒，调用方保证为正值。
    inline void set_attack_ms(float ms)
    {
        // 时间变更影响包络系数，不能只写毫秒数而遗漏脏标记。
        attack_ms.store(ms);
        needs_update = true;
    }
    /// @brief 设置下降包络的时间参数，单位毫秒，调用方保证为正值。
    inline void set_release_ms(float ms)
    {
        release_ms.store(ms);
        needs_update = true;
    }
    /// @brief 设置压缩后补偿增益，单位为 dB。
    inline void set_makeup_gain_db(float db)
    {
        makeup_gain_db.store(db);
        needs_update = true;
    }

protected:
    /// @brief 从输入复制 PCM 后按每声道包络计算压缩增益。
    /// @warning 每块处理；现有更新分支可能扩容，不能认定已经无分配。
    void apply_effect(AudioBuffer& output, const AudioBuffer& input) override;

private:
    /// @brief 控制侧设置、处理侧读取的电平阈值。
    /// @warning 原子字段不与 needs_update 形成事务，setter 不可并发执行。
    std::atomic<float> threshold_db{ 0.0f };
    /// @brief 超阈值压缩比，1 表示不压缩。
    std::atomic<float> ratio{ 1.0f };
    /// @brief 包络上升时间，转换系数时使用当前采样率。
    std::atomic<float> attack_ms{ 5.0f };
    /// @brief 包络下降时间，与上升时间独立配置。
    std::atomic<float> release_ms{ 100.0f };
    /// @brief 压缩后的增益补偿，当前实现逐样本读取。
    std::atomic<float> makeup_gain_db{ 0.0f };

    // 包络跟随器 (Envelope Follower) 的状态
    // 这个状态必须在 process 调用之间保持
    // 每个声道都需要自己的状态
    std::vector<double> envelope_db;

    // 根据 ms 计算出的,用于平滑的系数
    double attack_coeff       = 0.0;
    double release_coeff      = 0.0;
    double makeup_gain_linear = 1.0;

    /// @brief 上一次计算系数使用的采样率，变化时需重算毫秒到采样的转换。
    double sample_rate{ static_cast<double>(
        ICEConfig::internal_format.samplerate) };
    /// @brief 非原子更新标志，只允许处理停止时由控制接口置位。
    bool needs_update = true;
};
}  // namespace ice

#endif  // ICE_COMPRESSER_HPP
