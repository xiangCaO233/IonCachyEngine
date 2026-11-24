#pragma once

#include <ice/core/effect/IEffectNode.hpp>

namespace ice
{
/// @brief 无状态硬削波，将各声道样本限制在 [-1, 1]。
/// @details 保留范围内的样本；正负无穷饱和到对应边界，NaN 转为静音。
class Clipper : public IEffectNode
{
public:
    /// @brief 使用基类默认准备流程，不创建额外的限幅状态。
    Clipper();
    /// @brief 仅释放基类管理的输入节点及缓冲。
    ~Clipper() override = default;

protected:
    /// @brief 用裁剪后的输入覆盖所有活动帧。
    /// @param output 格式和活动帧数已由基类检查的目标缓冲。
    /// @param input 与目标分离的上游 PCM 缓冲。
    /// @warning 每个音频块调用；不得分配、加锁或更改缓冲容量。
    void apply_effect(AudioBuffer& output, const AudioBuffer& input) override;
};

}  // namespace ice
