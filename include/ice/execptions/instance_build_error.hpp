#pragma once

#include <stdexcept>

namespace ice
{
/// @brief 解码实例创建失败的历史错误类型。
/// 缓存解码任务在工厂返回空实例时使用，文本包含待加载的路径。
/// 它不拥有工厂或失败实例，后台任务的结果通道负责传递失败。
/// @warning 仅兼容既有异常调用链，新接口应通过返回值表达失败。
/// 类型没有额外状态，诊断文本的生命周期遵循 runtime_error。
class instance_build_error : public std::runtime_error
{
    using std::runtime_error::runtime_error;
};
}  // namespace ice
