#include "ALPlayerPlatform.h"

// 原生 API 只在此翻译单元可见，避免向共用播放器传递 Windows 类型。
#include <windows.h>

#include <cstdlib>

namespace ice::OpenALPlatform
{
namespace
{
/// @brief 对未设置或为空的配置提供进程级默认值。
/// @param name OpenAL Soft 环境变量名。
/// @param value 设备首次初始化前读取的默认字符串。
void set_environment_default(const char* name, const char* value)
{
    const char* current = std::getenv(name);
    if ( !current || !*current ) {
        // 通过 CRT 写入，与 OpenAL Soft 读取同进程配置的原行为一致。
        // 环境写入失败仍允许设备打开自行回退和报告错误。
        _putenv_s(name, value);
    }
}
}  // namespace

/// @brief Windows 保留既有驱动回退顺序，并设置缺省日志级别。
/// @note 两项独立检查，外部指定驱动不影响日志级别的初始化。
void configure_environment()
{
    set_environment_default("ALSOFT_DRIVERS", "winmm,dsound,wasapi");
    set_environment_default("ALSOFT_LOGLEVEL", "2");
}

/// @brief 仅提高当前供数线程的优先级，不改变控制线程或整个进程。
void configure_feed_thread() noexcept
{
    // GetCurrentThread 返回当前线程伪句柄，不需要关闭，也不交给控制侧。
    // 系统能拒绝调整；供数继续使用原优先级，策略不承诺实时调度。
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
}
}  // namespace ice::OpenALPlatform
