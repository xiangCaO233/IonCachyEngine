#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

/// @brief SDL 线程的不透明句柄，完整定义仅在实现文件可见。
struct SDL_Thread;

namespace ice
{
/// @brief 将低频后台任务分派到固定数量工作线程的队列。
/// @details 任务按入队顺序取出，但多个线程的实际开始及完成顺序不保证一致。
/// 队列不设容量上限，不提供取消、优先级或任务超时；不能当作音频实时调度器。
class ThreadPool
{
public:
    /// @brief 启动工作线程，构造完成后可接收任务。
    /// @param num_threads 请求线程数；零先取硬件并发数，最终最少创建四个线程。
    /// @details 负数、一至三个线程的请求也提升为四，不会创建单线程串行执行器。
    /// @warning
    /// 构造分配并启动线程，只能在准备阶段执行；容器分配仍可能失败。
    /// @details 线程创建失败会回收已创建线程并保持停止状态，后续提交被拒绝。
    ThreadPool(int32_t num_threads = 0);

    /// @brief 请求停止并等待所有已接收任务与工作线程结束。
    /// @pre 外部生产者必须先停止提交，不能依赖 m_stop
    /// 检查保护与析构并发的成员调用。
    /// @warning 低频收尾可能无限期
    /// join，任务必须能自行返回；没有强制取消或截止时间。
    /// 不得从本池工作线程销毁线程池，否则会尝试等待自身；也不得持有任务需要的外部锁。
    virtual ~ThreadPool();

    /// @brief 提交带结果任务，将执行结果保存在 future 共享状态中。
    /// @param f 待执行可调用对象。
    /// @param args 保存衰减后的参数值；独占资源可移动传入，显式引用须自行保活。
    /// @return 接收成功返回有效 future；停止状态拒绝时返回无共享状态的 future。
    /// @details packaged_task 捕获任务异常供 future 消费，停止拒绝不抛出。
    /// 入口已停止时不绑定函数或参数，调用方传入的独占资源保持原所有权。
    /// @warning 提交分配、复制共享所有权并竞争锁，禁止在音频回调使用。
    /// 同池任务等待新入队任务可耗尽全部工作线程，调用方必须避免这类依赖死锁。
    template<class F, class... Args>
    auto enqueue(F&& f, Args&&... args)
        -> std::future<typename std::invoke_result<F, Args...>::type>
    {
        using return_type = typename std::invoke_result<F, Args...>::type;

        {
            // 启动失败的池会永久停止，在移动参数和分配任务状态前直接拒绝。
            std::lock_guard<std::mutex> lock(m_queueMutex);
            if ( m_stop ) return {};
        }
        // 不持锁构造用户函数和参数，避免用户定义的复制/移动操作重入队列而死锁。
        // packaged_task 不可复制，共享包装使其能被 std::function
        // 保存；此成本发生在提交侧。
        auto invocation =
            [function   = std::forward<F>(f),
             parameters = std::tuple<std::decay_t<Args>...>(
                 std::forward<Args>(args)...)]() mutable -> return_type {
            /// @brief
            /// 优先以左值调用已存函数，必要时支持仅能右值调用的函数对象。
            auto invokeStored = [&function](auto&&... values) -> return_type {
                if constexpr ( std::is_invocable_r_v<return_type,
                                                     decltype(function)&,
                                                     decltype(values)...> ) {
                    return std::invoke_r<return_type>(
                        function, std::forward<decltype(values)>(values)...);
                } else {
                    return std::invoke_r<return_type>(
                        std::move(function),
                        std::forward<decltype(values)>(values)...);
                }
            };
            // 保留既有引用参数访问存储副本的行为；只有不能这样调用时才消费参数。
            // 每个 packaged_task
            // 只执行一次，移动独占参数后不会再次使用同一绑定。
            if constexpr ( std::is_invocable_r_v<return_type,
                                                 decltype(function)&,
                                                 std::decay_t<Args>&...> ||
                           std::is_invocable_r_v<return_type,
                                                 decltype(function)&&,
                                                 std::decay_t<Args>&...> ) {
                return std::apply(invokeStored, parameters);
            } else {
                // 独占参数需要移动时不能移动整个元组，否则同时需要 T&
                // 的参数失配。
                // 提交时的左值仍访问存储副本，右值才允许被本次任务消费。
                // 使用衰减类型保持数组和函数参数的既有存储规则，不恢复外部借用。
                /// @brief 按提交参数的值类别逐项转发一次任务所持有的参数。
                return [&]<std::size_t... Indices>(
                           std::index_sequence<Indices...>) -> return_type {
                    return invokeStored(
                        std::forward<
                            std::conditional_t<std::is_lvalue_reference_v<Args>,
                                               std::decay_t<Args>&,
                                               std::decay_t<Args>&&>>(
                            std::get<Indices>(parameters))...);
                }(std::index_sequence_for<Args...>{});
            }
        };
        auto task_ptr = std::make_shared<std::packaged_task<return_type()>>(
            std::move(invocation));

        // future
        // 的共享状态保存结果，不借用队列节点；池析构仍会等待任务执行结束。
        std::future<return_type> res = task_ptr->get_future();

        {
            std::unique_lock<std::mutex> lock(m_queueMutex);

            // 入队仍在同一锁内复查状态；入口检查不授权与析构并发访问对象。
            if ( m_stop ) {
                return {};
            }

            // 队列捕获保持任务存活；调用方丢弃返回的 future 不会取消此任务。
            m_tasks.emplace([task_ptr]() { (*task_ptr)(); });
        }

        m_condition.notify_one();

        return res;
    }

    /// @brief 提交无需结果句柄的普通任务，空任务允许入队并由工作线程跳过。
    /// @param task_func 被队列接管的任务，捕获的引用须覆盖实际执行期间。
    /// @details 不使用
    /// packaged_task，任务异常没有结果通道且会逃出工作线程入口。
    /// @return 接收成功为 true，停止状态拒绝为 false；不报告任务执行结果。
    /// @warning 分配和队列锁均不适合音频回调；不允许与线程池析构并发调用。
    bool enqueue_void(std::function<void()> task_func)
    {
        {
            std::unique_lock<std::mutex> lock(m_queueMutex);
            if ( m_stop ) {
                return false;
            }
            m_tasks.emplace(std::move(task_func));
        }
        m_condition.notify_one();
        return true;
    }

private:
    /// @brief 将 SDL 线程入口转发到仍存活的线程池。
    struct WorkerEntry;

    /// @brief 持续消费共享队列，停止时完成已接收任务后退出。
    /// @return 队列已排空且停止请求成立时返回零。
    /// @warning 后台等待可能阻塞，任务须自行结束，禁止作为实时音频回调。
    int workerLoop();

    /// @brief 发布停止并回收已创建线程，允许构造失败后析构再次调用。
    /// @warning 仅供构造失败及析构调用；等待无截止时间，不允许工作线程调用。
    void stopWorkers();

    /// @brief 固定工作线程集合，仅由构造和析构管理。
    std::vector<SDL_Thread*> m_workers;
    /// @brief 待执行任务的 FIFO，入队及取出均由 m_queueMutex 保护。
    std::queue<std::function<void()>> m_tasks;

    /// @brief 同时保护 m_tasks 与 m_stop，不保护用户任务内部的数据。
    std::mutex m_queueMutex;
    /// @brief 唤醒等待任务或停止请求的工作线程，通知本身不携带任务内容。
    std::condition_variable m_condition;
    /// @brief 持队列锁读写的终止请求，不代表工作线程均已退出。
    bool m_stop{ false };
};
}  // namespace ice
