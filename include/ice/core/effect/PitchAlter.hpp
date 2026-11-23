#pragma once

#include <ice/core/effect/IEffectNode.hpp>
#include <ice/core/effect/rubberband/RStretcher.hpp>

#include <cstddef>
#include <memory>

namespace ice
{
/// @brief 独立调整音高，保持时间倍率及上游请求帧数不变。
/// @details 处理容量、格式限制和输入复用由 IEffectNode 统一执行。
/// 单位倍率附近保留累加直通；非单位倍率由控制侧准备的后端写满输出。
class PitchAlter : public IEffectNode
{
public:
    /// @brief 采用基类已准备的默认音频格式和设备块长度。
    PitchAlter();

    /// @brief 设置音高倍率并预热相应配置；非法倍率保持原配置。
    /// @warning 仅可在处理停止时调用，切换倍率可重建后端并丢弃旧处理历史。
    void set_pitch_scale(double scale);

    /// @brief 用半音设置音高，十二半音对应倍率翻倍。
    /// @warning 与 set_pitch_scale 一样，只允许在处理停止时重配。
    void set_pitch_shift(double semitones);

    /// @brief 查询已配置的半音数；禁止与控制侧重配并发。
    [[nodiscard]] double semitones() const { return m_pitchSemitones; }

    /// @brief 查询已配置的倍率；禁止与控制侧重配并发。
    [[nodiscard]] double scale() const { return m_pitchScale; }

    /// @brief 通过基类检查已连接的上游，未连接时保留输出。
    /// @warning 音频热路径，每块调用，不持锁、不分配，不复制上游所有权。
    void process(AudioBuffer& buffer) override;

protected:
    /// @brief 对已验证的输入执行累加直通或固定倍率的变调。
    /// @warning 音频热路径，每次处理一块；禁止调整格式、容量或倍率。
    void apply_effect(AudioBuffer& output, const AudioBuffer& input) override;

private:
    /// @brief 从基类成功准备的输入块记录配置，重新建立变调器。
    /// @warning 控制侧可能分配；禁止与 process 并发。
    void prepare_effect(const AudioDataFormat& format) override;

    /// @brief 应用一组等价的倍率和半音参数，倍率不变时保留算法历史。
    void updatePitch(double scale, double semitones);

    /// @brief 按最新成功准备的块尺寸及最终倍率构造和预热后端。
    void prepareStretcher();

    /// @brief 控制侧最近成功准备的格式，用于停机改倍率时复用。
    AudioDataFormat m_preparedFormat{};
    /// @brief 控制侧准备的最大输入与输出帧数，不随短块变化。
    std::size_t m_preparedFrames{ 0U };
    /// @brief 控制侧配置的半音表示。
    double m_pitchSemitones{ 0.0 };
    /// @brief 控制侧配置的音高倍率。
    double m_pitchScale{ 1.0 };
    /// @brief 与准备格式及倍率匹配的预热后端；空表示直通或未经有效准备。
    std::unique_ptr<RStretcher> m_stretcher;
};
}  // namespace ice
