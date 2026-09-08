#ifndef ICE_THREADPOOL_HPP
#define ICE_THREADPOOL_HPP

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <future>
#include <queue>
#include <thread>
#include <vector>

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
    /// 构造分配并启动线程，只能在准备阶段执行；部分创建失败没有显式回滚逻辑。
    ThreadPool(int32_t num_threads = 0)
    {
        // 硬件并发提示可能返回零，下方下限仍负责选择可运行的默认规模。
        if ( num_threads == 0 ) {
            num_threads = std::thread::hardware_concurrency();
        }

        if ( num_threads < 4 ) num_threads = 4;

        // 工作线程借用 this；线程池地址与成员必须存活到析构中的全部 join 完成。
        for ( size_t i = 0; i < num_threads; ++i ) {
            workers.emplace_back([this] {
                while ( true ) {
                    // 从共享队列转移到线程局部后执行，任务捕获保持到本轮结束。
                    std::function<void()> task;
                    {
                        std::unique_lock<std::mutex> lock(queue_mutex);

                        // 谓词在持锁时检查，应对伪唤醒；休眠期间释放队列锁供生产者入队。
                        // 这是后台工作线程的空闲等待，不允许搬入音频拉取调用链。
                        this->condition.wait(
                            lock, [this] { return stop || !tasks.empty(); });

                        // 停止请求不取消已排队任务，只有共享队列已排空才退出。
                        if ( stop && tasks.empty() ) {
                            return;
                        }

                        if ( !tasks.empty() ) {
                            // 队首任务只归一个工作线程；移动后立刻出队，不在锁内运行用户代码。
                            task = std::move(tasks.front());
                            tasks.pop();
                        } else {
                            // 持锁谓词成立后正常不应到此；保留原防御分支，不执行空任务。
                            continue;
                        }
                    }

                    // 执行期间不持队列锁，允许任务继续提交任务，但不保证嵌套等待能推进。
                    // 普通任务异常没有在此捕获；enqueue 的 packaged_task 与
                    // void 入口行为不同。
                    if ( task ) {
                        task();
                    }
                }
            });
        }
    }

    /// @brief 请求停止并等待所有已接收任务与工作线程结束。
    /// @pre 外部生产者必须先停止提交，不能依赖 stop
    /// 检查保护与析构并发的成员调用。
    /// @warning 低频收尾可能无限期
    /// join，任务必须能自行返回；没有强制取消或截止时间。
    /// 不得从本池工作线程销毁线程池，否则会尝试等待自身；也不得持有任务需要的外部锁。
    virtual ~ThreadPool()
    {
        {
            std::unique_lock<std::mutex> lock(queue_mutex);
            // 与入队及取队列使用同一把锁，停止判定不需要额外原子变量。
            stop = true;
        }

        // 所有空闲线程都要重新检查终止条件，不能只唤醒一个后留下其余线程等待。
        condition.notify_all();

        // 队列为空不表示已取出的任务完成，仍须逐个 join 后才能销毁成员。
        for ( std::thread& worker : workers ) {
            if ( worker.joinable() ) {
                worker.join();
            }
        }
    }
    /// @brief 提交带结果任务，将执行结果保存在 future 共享状态中。
    /// @param f 待执行可调用对象。
    /// @param args 绑定参数；默认由 std::bind
    /// 保存衰减后的值，显式引用绑定需自行保活。
    /// @return 用于一次性取得结果的 future；返回本身不表示任务已开始。
    /// @details packaged_task 捕获任务异常供 future
    /// 消费，历史停止分支仍直接抛出异常。
    /// @warning 提交分配、复制共享所有权并竞争锁，禁止在音频回调使用。
    /// 同池任务等待新入队任务可耗尽全部工作线程，调用方必须避免这类依赖死锁。
    template<class F, class... Args>
    auto enqueue(F&& f, Args&&... args)
        -> std::future<typename std::invoke_result<F, Args...>::type>
    {
        using return_type = typename std::invoke_result<F, Args...>::type;

        // packaged_task 不可复制，共享包装使其能被 std::function
        // 保存；此成本发生在提交侧。
        auto task_ptr = std::make_shared<std::packaged_task<return_type()>>(
            std::bind(std::forward<F>(f), std::forward<Args>(args)...));

        // future
        // 的共享状态保存结果，不借用队列节点；池析构仍会等待任务执行结束。
        std::future<return_type> res = task_ptr->get_future();

        {
            std::unique_lock<std::mutex> lock(queue_mutex);

            // 这里的拒绝检查晚于绑定和任务分配，不保证失败提交没有分配或参数移动。
            if ( stop ) {
                throw std::runtime_error("enqueue on stopped TaskExecutor");
            }

            // 队列捕获保持任务存活；调用方丢弃返回的 future 不会取消此任务。
            tasks.emplace([task_ptr]() { (*task_ptr)(); });
        }

        condition.notify_one();

        return res;
    }

    /// @brief 提交无需结果句柄的普通任务，空任务允许入队并由工作线程跳过。
    /// @param task_func 被队列接管的任务，捕获的引用须覆盖实际执行期间。
    /// @details 不使用
    /// packaged_task，任务异常没有结果通道且会逃出工作线程入口。
    /// 历史停止分支通过异常拒绝提交，没有返回值式错误报告。
    /// @warning 分配和队列锁均不适合音频回调；不允许与线程池析构并发调用。
    void enqueue_void(std::function<void()> task_func)
    {
        {
            std::unique_lock<std::mutex> lock(queue_mutex);
            if ( stop ) {
                throw std::runtime_error(
                    "enqueue_void on stopped TaskExecutor");
            }
            tasks.emplace(std::move(task_func));
        }
        condition.notify_one();
    }

private:
    /// @brief 固定工作线程集合，仅由构造和析构管理。
    std::vector<std::thread> workers;
    /// @brief 待执行任务的 FIFO，入队及取出均由 queue_mutex 保护。
    std::queue<std::function<void()>> tasks;

    /// @brief 同时保护 tasks 与 stop，不保护用户任务内部的数据。
    std::mutex queue_mutex;
    /// @brief 唤醒等待任务或停止请求的工作线程，通知本身不携带任务内容。
    std::condition_variable condition;
    /// @brief 持队列锁读写的终止请求，不代表工作线程均已退出。
    bool stop{ false };
};
}  // namespace ice

#endif  // ICE_THREADPOOL_HPP
