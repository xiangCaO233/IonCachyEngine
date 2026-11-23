#pragma once

#include <vector>

#include "ice/config/config.hpp"
#include "ice/core/effect/IEffectNode.hpp"
namespace ice
{

/// @brief 各声道独立维护包络的动态范围压缩器。
/// @details 各声道不联动，立体声两路可能获得不同衰减；不是峰值限幅器。
/// 参数未做有限性与范围校验，调用方须提供有限阈值、增益和有效时间、比率。
/// @warning 所有 setter 与 prepare 只能在接入音频图以前或处理已停止时调用。
/// 恢复处理前由宿主发布配置；处理期间参数保持不变，不能跨线程并发读写。
class Compressor : public IEffectNode
{
public:
    /// @brief 初始化默认格式对应的声道包络。
    Compressor();

    /// @brief 设置开始压缩的电平阈值，单位为 dB。
    inline void set_threshold_db(float db)
    {
        // 阈值仅用于每块增益计算，调整它不改变包络系数。
        threshold_db = db;
    }
    /// @brief 设置超阈值部分的压缩比；调用方保证数值有效且非零。
    inline void set_ratio(float vratio) { ratio = vratio; }
    /// @brief 设置上升包络的时间参数，单位毫秒，调用方保证为正值。
    inline void set_attack_ms(float ms)
    {
        // 时间变更影响包络系数，不能只写毫秒数而遗漏脏标记。
        attack_ms    = ms;
        needs_update = true;
    }
    /// @brief 设置下降包络的时间参数，单位毫秒，调用方保证为正值。
    inline void set_release_ms(float ms)
    {
        release_ms   = ms;
        needs_update = true;
    }
    /// @brief 设置压缩后补偿增益，单位为 dB。
    inline void set_makeup_gain_db(float db)
    {
        // dB 值在块边界取快照，补偿与包络的时间系数互相独立。
        makeup_gain_db = db;
    }

protected:
    /// @brief 从输入复制 PCM 后按每声道包络计算压缩增益。
    /// @warning 每块处理；只读取控制侧已冻结参数并更新预分配声道状态。
    /// 处理期间不允许 setter 写入，不得为参数访问增加锁或原子操作。
    void apply_effect(AudioBuffer& output, const AudioBuffer& input) override;

private:
    /// @brief 调整包络状态以匹配新格式，并保留仍存在的声道历史。
    /// @warning 仅在控制侧停止处理后调用，增大声道数可能分配。
    void prepare_effect(const AudioDataFormat& format) override;

    /// @brief 触发增益衰减的电平阈值，由控制侧设置、处理侧每块读取。
    float threshold_db{ 0.0f };
    /// @brief 超阈值压缩比，1 表示不压缩，小于一时可能增大超阈值部分。
    float ratio{ 1.0f };
    /// @brief 包络上升时间，毫秒；只在采样率或时间常数变化后重算系数。
    float attack_ms{ 5.0f };
    /// @brief 包络下降时间，毫秒；与上升时间独立配置。
    float release_ms{ 100.0f };
    /// @brief 压缩后补偿增益，单位为 dB，处理侧每块读取一次。
    float makeup_gain_db{ 0.0f };

    /// @brief 跨块延续的各声道 dB 包络，新增元素从零 dB 开始而非静音电平。
    /// @warning 仅 prepare_effect 可调整尺寸，逐块处理只更新元素。
    std::vector<double> envelope_db;

    /// @brief 电平高于历史包络时使用的单样本平滑系数。
    double attack_coeff = 0.0;
    /// @brief 电平不高于历史包络时使用的单样本平滑系数。
    double release_coeff = 0.0;
    /// @brief 上一次计算系数使用的采样率，变化时需重算毫秒到采样的转换。
    double sample_rate{ static_cast<double>(
        ICEConfig::internal_format.samplerate) };
    /// @brief
    /// 包络时间系数的更新标志，控制侧修改时间常数后置位，处理侧下块消费。
    bool needs_update = true;
};
}  // namespace ice
