#ifndef ICE_ALLOCATIONTRACKER_HPP
#define ICE_ALLOCATIONTRACKER_HPP

#pragma once
#include <atomic>
#include <iostream>
#include <new>

/// @brief 测试进程中普通全局 new 的调用次数。
/// @warning 多线程写、诊断代码读；relaxed 只累计次数，不发布分配对象。
/// 本头定义全局替换函数，只能由单个翻译单元包含，不能进入公共聚合头。
static std::atomic<size_t> g_alloc_count = 0;
/// @brief 被本头覆盖的 delete 重载调用次数，不等价于释放成功的对象数。
static std::atomic<size_t> g_dealloc_count = 0;

/// @brief 记录普通分配入口，再转发给 C 堆。
/// 对齐、数组及第三方直接调用 C 堆的行为不应假定都由此入口覆盖。
[[nodiscard]] void* operator new(size_t size)
{
    // 失败前也会计数，因此统计的是尝试次数，不是成功分配次数。
    g_alloc_count.fetch_add(1, std::memory_order_relaxed);
    // 沿用普通 new 的历史失败路径；这个测试替换入口不提供错误恢复。
    void* p = malloc(size);
    if ( !p ) throw std::bad_alloc();
    return p;
}

/// @brief 记录普通释放入口；指针释放语义由 C 堆承担。
void operator delete(void* p) noexcept
{
    g_dealloc_count.fetch_add(1, std::memory_order_relaxed);
    // 计数发生在释放入口，不能把它当成堆分配与释放一一配对的记录。
    free(p);
}

/// @brief 覆盖编译器可选择的带大小释放入口，避免漏计这一路调用。
void operator delete(void* p, size_t size) noexcept
{
    // 此统计不追踪分配字节数，大小参数也不能用于推导当前存活对象数。
    (void)size;  // size可能未使用，避免警告
    g_dealloc_count.fetch_add(1, std::memory_order_relaxed);
    free(p);
}

namespace ice
{

/// @brief 开启新的统计区间，调用前必须停止被测任务的分配活动。
/// @warning 两个计数独立复位，不能与工作线程并发重置后声称是完整区间。
inline void reset_allocation_counters()
{
    // 只复位观测值，不释放任何对象，也不记录区间开始前的存量。
    g_alloc_count   = 0;
    g_dealloc_count = 0;
}

/// @brief 在测试结束后输出计数快照，供人工诊断。
/// @warning 流输出可能加锁或分配，禁止在实时音频路径调用。
inline void print_allocation_stats()
{
    // 两次独立原子读取不构成事务快照；应先停止任务再打印最终结果。
    std::cout << "[分配统计] New 调用数: " << g_alloc_count
              << ", delete 调用数: " << g_dealloc_count << "\n";
}

}  // namespace ice

#endif  // ICE_ALLOCATIONTRACKER_HPP
