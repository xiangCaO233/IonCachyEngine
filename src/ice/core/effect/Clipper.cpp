#include <ice/core/effect/Clipper.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace ice
{
/// @brief 沿用基类的缓冲准备，固定阈值不需要额外状态。
Clipper::Clipper() = default;

/// @brief 逐声道写出当前块的硬削波结果。
/// @param output 允许覆盖的活动目标平面。
/// @param input 从上游取得的只读样本平面。
/// @pre 基类已验证两个缓冲区格式和活动长度一致，且它们不重叠。
/// @details 固定阈值不使用跨块包络，适用于归一化浮点 PCM。
/// @warning 音频热路径只访问基类准备好的活动样本，不读写容量尾部。
void Clipper::apply_effect(AudioBuffer& output, const AudioBuffer& input)
{
    const auto frames = input.num_frames();
    // 空块没有活动样本，直接返回也允许其平面表尚未暴露给调用方。
    if ( frames == 0U ) return;

    const auto inputs  = input.raw_ptrs();
    const auto outputs = output.raw_ptrs();
    // 基类只初始化输入；必须显式覆盖全部活动输出以防止复用旧块。
    for ( std::size_t channel = 0U; channel < input.num_channels();
          ++channel ) {
        // 各声道独立使用自己的跨度，不把分配器的对齐尾部算作 PCM。
        const auto* source = inputs[channel];
        auto*       target = outputs[channel];
        for ( std::size_t frame = 0U; frame < frames; ++frame ) {
            const auto sample = source[frame];
            // NaN 没有顺序，单独还原为静音；无穷由同一限幅边界收敛。
            target[frame] =
                std::isnan(sample) ? 0.0F : std::clamp(sample, -1.0F, 1.0F);
        }
    }
}
}  // namespace ice
