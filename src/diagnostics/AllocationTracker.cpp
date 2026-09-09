#include <ice/tool/AllocationTracker.hpp>

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <new>

/// @brief 测试进程中普通全局 new 的调用次数。
/// @warning 多线程写、诊断代码读；relaxed 只累计次数，不发布分配对象。
/// 替换定义仅链接到显式启用诊断的可执行文件，不能编入引擎库。
/// 没有线程局部开关，链接到该入口的准备、后台任务及诊断分配都会累计。
std::atomic<size_t> g_alloc_count = 0;
/// @brief 被诊断实现覆盖的 delete 重载调用次数，不等价于释放成功的对象数。
/// @warning 多线程释放侧以 relaxed
/// 累加，诊断侧读取；仅作统计，不同步被释放对象的访问。
std::atomic<size_t> g_dealloc_count = 0;

/// @brief 记录普通分配入口，再转发给 C 堆。
/// @param size 请求字节数，零字节按一个字节申请以满足普通分配契约。
/// @return 成功分配的存储；原有失败分支抛出 bad_alloc，尚非返回值式错误接口。
/// @warning 所有进入此入口的线程均执行 relaxed
/// 原子累加，计数器竞争会影响被测时延。 对齐、数组及第三方直接调用 C
/// 堆的行为不应假定都由此入口覆盖。
[[nodiscard]] void* operator new(size_t size)
{
    // 失败前也会计数，因此统计的是尝试次数，不是成功分配次数。
    g_alloc_count.fetch_add(1, std::memory_order_relaxed);
    // 沿用普通 new 的历史失败路径；这个测试替换入口不提供错误恢复。
    // malloc(0) 允许返回空指针，但零尺寸的普通分配请求不能因此被误判失败。
    void* p = std::malloc(size == 0 ? 1 : size);
    if ( !p ) throw std::bad_alloc();
    return p;
}

/// @brief 记录普通释放入口；指针释放语义由 C 堆承担。
/// @param p 原分配地址；本入口被直接调用时也计入空指针调用。
/// @warning 释放侧 relaxed 原子累加只记录入口事件，不能证明析构或回收已完成。
void operator delete(void* p) noexcept
{
    g_dealloc_count.fetch_add(1, std::memory_order_relaxed);
    // 计数发生在释放入口，不能把它当成堆分配与释放一一配对的记录。
    std::free(p);
}

/// @brief 覆盖编译器可选择的带大小释放入口，避免漏计这一路调用。
/// @param p 原分配地址。
/// @param size 编译器提供的大小，本实现不校验也不累计它。
/// @warning 释放侧以 relaxed 累加，直接转发 free，避免再调用另一 delete
/// 重载重复计数。
void operator delete(void* p, size_t size) noexcept
{
    // 此统计不追踪分配字节数，大小参数也不能用于推导当前存活对象数。
    (void)size;
    g_dealloc_count.fetch_add(1, std::memory_order_relaxed);
    std::free(p);
}

namespace ice
{

/// @brief 开启新的统计区间，调用前必须停止被测任务的分配活动。
/// @warning 两个计数独立复位，不能与工作线程并发重置后声称是完整区间。
/// relaxed 只复位诊断计数，不负责发布对象或同步被测线程。
void reset_allocation_counters()
{
    // 只复位观测值，不释放任何对象，也不记录区间开始前的存量。
    g_alloc_count.store(0, std::memory_order_relaxed);
    g_dealloc_count.store(0, std::memory_order_relaxed);
}

/// @brief 在测试结束后输出计数快照，供人工诊断。
/// @warning 流输出可能加锁或分配，禁止在实时音频路径调用。
/// @details
/// 输出操作自身也可能影响全局计数，结果只供粗粒度诊断，不是无扰动测量。
/// 这里保留历史 cout 入口，尚未迁移项目日志规范；不要作为新增诊断代码模板。
void print_allocation_stats()
{
    // 两次独立原子读取不构成事务快照；应先停止任务再打印最终结果。
    // 在任何流操作前保存两个值，避免输出自身的分配或释放污染本次报告。
    // relaxed 读取只观测计数，工作线程停止仍须依靠调用方的生命周期同步。
    const size_t allocationCount =
        g_alloc_count.load(std::memory_order_relaxed);
    const size_t deallocationCount =
        g_dealloc_count.load(std::memory_order_relaxed);
    std::cout << "[分配统计] New 调用数: " << allocationCount
              << ", delete 调用数: " << deallocationCount << "\n";
}

}  // namespace ice
