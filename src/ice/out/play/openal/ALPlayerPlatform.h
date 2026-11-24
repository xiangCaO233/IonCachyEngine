#pragma once

namespace ice::OpenALPlatform
{
/// @brief 在打开设备前设置 OpenAL Soft 的进程环境默认值。
/// @details 非空外部配置优先，空值与未设置均使用平台默认。
/// @pre 控制侧串行初始化，不允许并发写入进程环境。
void configure_environment();

/// @brief 使用平台可用的线程策略，失败时保留系统调度。
/// @warning 仅在供数线程入口调用一次，禁止在逐块路径调用。
void configure_feed_thread() noexcept;
}  // namespace ice::OpenALPlatform
