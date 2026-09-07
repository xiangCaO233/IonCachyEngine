#ifndef ICE_LOAD_ERROR_HPP
#define ICE_LOAD_ERROR_HPP

#include <stdexcept>
namespace ice
{
/// @brief 媒体打开、定位及解码初始化失败的历史错误类型。
/// 诊断文本可能来自 FFmpeg 返回值，也可能由解码器补充上下文。
/// 文本不等同于稳定错误码，调用方不能依赖字符串选择恢复策略。
/// @warning 仅兼容既有异常调用链，新接口应通过返回值表达失败。
/// 类型没有额外状态，诊断文本的生命周期遵循 runtime_error。
class load_error : public std::runtime_error
{
    using std::runtime_error::runtime_error;
};
}  // namespace ice

#endif  // ICE_LOAD_ERROR_HPP
