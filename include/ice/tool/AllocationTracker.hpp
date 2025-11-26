#ifndef ICE_ALLOCATIONTRACKER_HPP
#define ICE_ALLOCATIONTRACKER_HPP

#pragma once
#include <atomic>
#include <iostream>
#include <new>

// 使用原子变量，确保在多线程测试中计数器是线程安全的
static std::atomic<size_t> g_alloc_count   = 0;
static std::atomic<size_t> g_dealloc_count = 0;

// 重载全局 operator new
[[nodiscard]] void* operator new(size_t size)
{
    g_alloc_count.fetch_add(1, std::memory_order_relaxed);
    void* p = malloc(size);
    if ( !p ) throw std::bad_alloc();
    return p;
}

// 重载全局 operator delete
void operator delete(void* p) noexcept
{
    g_dealloc_count.fetch_add(1, std::memory_order_relaxed);
    free(p);
}

// C++14及以后，通常会调用带大小的delete
void operator delete(void* p, size_t size) noexcept
{
    (void)size;  // size可能未使用，避免警告
    g_dealloc_count.fetch_add(1, std::memory_order_relaxed);
    free(p);
}

namespace ice
{

inline void reset_allocation_counters()
{
    g_alloc_count   = 0;
    g_dealloc_count = 0;
}

inline void print_allocation_stats()
{
    std::cout << "[分配统计] New 调用数: " << g_alloc_count
              << ", delete 调用数: " << g_dealloc_count << "\n";
}

}  // namespace ice

#endif  // ICE_ALLOCATIONTRACKER_HPP
