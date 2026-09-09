#pragma once

#include <stdexcept>
namespace ice
{
/// @brief 音频缓冲区的历史格式/容量错误类型。
/// 调用点用于拒绝不兼容的缓冲访问或混音，不携带可恢复的 PCM 数据。
/// 错误文本由 runtime_error 保存，诊断者不应依赖文本解析格式字段。
/// @warning 仅兼容既有异常调用链，新接口应通过返回值表达失败。
/// 类型没有额外状态，诊断文本的生命周期遵循 runtime_error。
class buffer_error : public std::runtime_error
{
    using std::runtime_error::runtime_error;
};
}  // namespace ice
