#ifndef ICE_CLIPPER_HPP
#define ICE_CLIPPER_HPP

#include <ice/core/effect/IEffectNode.hpp>

namespace ice
{
/// @brief 限幅效果的占位类型，目前没有裁剪或直通实现。
/// 不能把该类型已存在当成输出采样已被限制到安全范围的依据。
class Clipper : public IEffectNode
{
public:
    /// @brief 使用基类默认准备流程，不创建额外的限幅状态。
    Clipper();
    /// @brief 仅释放基类管理的输入节点及缓冲。
    ~Clipper() override = default;

protected:
    /// @brief 当前不写输出，不能依赖此节点实现峰值保护。
    /// @warning 每块热路径；占位实现不代表已满足限幅算法需求。
    /// 输出不会自动复制输入，调用方不能将其视作直通效果。
    void apply_effect(AudioBuffer& output, const AudioBuffer& input) override;
};

}  // namespace ice

#endif  // ICE_CLIPPER_HPP
