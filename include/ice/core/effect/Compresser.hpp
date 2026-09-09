#pragma once

#include <atomic>

#include "ice/config/config.hpp"
#include "ice/core/effect/IEffectNode.hpp"
namespace ice
{

/// @brief 各声道独立维护包络的动态范围压缩器。
/// @details 各声道不联动，立体声两路可能获得不同衰减；不是峰值限幅器。
/// 参数未做有限性与范围校验，调用方须提供有限阈值、增益和有效时间、比率。
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
    /// @warning
    /// 阈值与比率每块读取，时间参数在更新分支读取，补偿增益逐样本读取； setter
    /// 是写入者，音频处理是读取者，当前均使用默认顺序一致原子操作。
    /// 这些历史原子未消除脏标记竞争，也未证明不可替代，本批不改变同步策略。
    void apply_effect(AudioBuffer& output, const AudioBuffer& input) override;

private:
    /// @brief 控制侧设置、处理侧读取的电平阈值。
    /// @warning 原子字段不与 needs_update 形成事务，setter 不可并发执行。
    std::atomic<float> threshold_db{ 0.0f };
    /// @brief 超阈值压缩比，1 表示不压缩。
    /// @warning setter
    /// 写、处理侧每块读，默认顺序一致；小于一时可能增大超阈值部分。
    std::atomic<float> ratio{ 1.0f };
    /// @brief 包络上升时间，转换系数时使用当前采样率。
    /// @warning setter
    /// 写、处理侧更新系数时读，默认顺序一致；不与脏标记构成事务。
    std::atomic<float> attack_ms{ 5.0f };
    /// @brief 包络下降时间，与上升时间独立配置。
    /// @warning setter
    /// 写、处理侧更新系数时读，默认顺序一致；不与脏标记构成事务。
    std::atomic<float> release_ms{ 100.0f };
    /// @brief 压缩后的增益补偿，当前实现逐样本读取。
    /// @warning setter
    /// 写、处理侧逐样本及更新分支读，默认顺序一致；不能用于多参数同步。
    std::atomic<float> makeup_gain_db{ 0.0f };

    /// @brief 跨块延续的各声道 dB 包络，新增元素从零 dB 开始而非静音电平。
    /// @warning 更新分支可能
    /// resize；声道数单独变化不会触发该分支，需保持准备格式稳定。
    std::vector<double> envelope_db;

    /// @brief 电平高于历史包络时使用的单样本平滑系数。
    double attack_coeff = 0.0;
    /// @brief 电平不高于历史包络时使用的单样本平滑系数。
    double release_coeff = 0.0;
    /// @brief 更新时计算的线性补偿缓存，当前逐样本实现并未使用此成员。
    double makeup_gain_linear = 1.0;

    /// @brief 上一次计算系数使用的采样率，变化时需重算毫秒到采样的转换。
    double sample_rate{ static_cast<double>(
        ICEConfig::internal_format.samplerate) };
    /// @brief 非原子更新标志，只允许处理停止时由控制接口置位。
    bool needs_update = true;
};
}  // namespace ice
