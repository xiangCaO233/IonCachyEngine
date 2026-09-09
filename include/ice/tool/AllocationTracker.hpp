#pragma once

#include <atomic>
#include <cstddef>

/// @brief 诊断进程普通分配入口的尝试次数，不等同于成功分配数。
/// @warning 分配线程以 relaxed 累加；诊断读取不发布被分配对象。
/// 仅声明共享计数，包含本头不会替换宿主的分配器。
/// 使用统计接口的诊断目标必须且只能链接一份 AllocationTracker.cpp。
extern std::atomic<size_t> g_alloc_count;
/// @brief 被诊断替换入口观测到的释放调用数，不保证覆盖所有分配种类。
/// @warning 释放线程以 relaxed 累加，不同步对象访问或析构完成状态。
extern std::atomic<size_t> g_dealloc_count;

namespace ice
{
/// @brief 在被测任务停止分配后重置两个共享计数。
/// @warning 低频诊断入口；两个 relaxed 写入不构成事务快照，禁止并发重置。
void reset_allocation_counters();

/// @brief 输出停止被测任务后的计数快照，供人工诊断。
/// @warning 输出可能分配或阻塞，只能在非实时诊断路径调用。
/// 输出前以 relaxed
/// 读取两项计数，避免输出自身事件改变本次报告；不提供跨线程同步。
/// 全局替换、输出和计数实现由诊断可执行文件单独链接，不由引擎库提供。
void print_allocation_stats();
}  // namespace ice
