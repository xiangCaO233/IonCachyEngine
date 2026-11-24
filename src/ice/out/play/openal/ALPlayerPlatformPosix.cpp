#include "ALPlayerPlatform.h"

#include <cstdlib>

namespace ice::OpenALPlatform
{
/// @brief POSIX 不指定驱动顺序，保留 OpenAL Soft 的系统选择。
void configure_environment()
{
    // getter 的返回值只在本次判断中借用，更新环境后不能继续访问。
    const char* current = std::getenv("ALSOFT_LOGLEVEL");
    if ( !current || !*current ) {
        // 已确认值为空或未设置，允许覆盖空项以匹配 Windows 语义。
        // 环境写入失败不阻止初始化，实际设备可用性仍由设备打开判定。
        setenv("ALSOFT_LOGLEVEL", "2", 1);
    }
}

/// @brief POSIX 供数保留继承的系统调度，初始化无需提高权限。
/// @note 默认设备选择由 OpenAL Soft 管理，供数入口不探测驱动。
void configure_feed_thread() noexcept
{
    // 这里不申请实时调度或设置 CPU 亲和性，避免平台适配改变现有优先级。
}
}  // namespace ice::OpenALPlatform
