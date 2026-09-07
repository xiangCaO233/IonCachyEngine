#include <ice/core/effect/Clipper.hpp>

namespace ice
{
/// @brief 只执行基类默认准备，不预设阈值或其他限幅参数。
Clipper::Clipper() {}

/// @brief 占位处理不修改输出，尚未提供音量裁剪算法。
/// @warning 音频热路径空实现；输入不会被自动直通到输出。
void Clipper::apply_effect(AudioBuffer& output, const AudioBuffer& input) {}

}  // namespace ice
