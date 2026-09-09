#include <ice/thread/ThreadPool.hpp>

#include <SDL3/SDL_thread.h>

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <utility>
#include <vector>

namespace ice
{
/// @brief 隐藏平台调用约定，公开头无需包含 SDL 线程接口。
struct ThreadPool::WorkerEntry {
    /// @brief 将线程入口转发到其借用的池实例。
    /// @param userdata 构造传入的池地址，停止流程等待访问结束后才能销毁。
    /// @return 队列排空并退出后返回零。
    static int SDLCALL run(void* userdata)
    {
        return static_cast<ThreadPool*>(userdata)->workerLoop();
    }
};

/// @brief 准备固定句柄容量并启动工作线程，失败后保留停止状态。
/// @param num_threads 请求数量，零采用硬件提示，最终至少四个线程。
/// @warning 低频初始化会分配；线程失败回收可能等待已创建线程退出。
ThreadPool::ThreadPool(int32_t num_threads)
{
    // 硬件并发提示可能返回零，下方下限仍负责选择可运行的默认规模。
    if ( num_threads == 0 ) {
        num_threads = std::thread::hardware_concurrency();
    }

    if ( num_threads < 4 ) num_threads = 4;

    // 先完成句柄容器分配，线程启动后 push_back 不再因扩容失败遗留活动线程。
    m_workers.reserve(static_cast<std::size_t>(num_threads));
    // SDL 以空句柄报告线程创建失败，不依赖 std::thread 的异常失败通道。
    for ( int32_t i = 0; i < num_threads; ++i ) {
        // 入口仅借用池地址；构造返回之前即可能运行，因此全部同步成员已先初始化。
        SDL_Thread* worker =
            SDL_CreateThread(WorkerEntry::run, "ICE task worker", this);
        if ( !worker ) {
            // 构造尚未发布对象，不会有外部任务；唤醒并回收此前创建的线程。
            stopWorkers();
            return;
        }
        m_workers.push_back(worker);
    }
}

/// @brief 停止并等待所有已接收任务完成，随后释放成员资源。
/// @warning 不允许在本池工作线程或持有任务所需外部锁时析构。
ThreadPool::~ThreadPool()
{
    stopWorkers();
}

/// @brief 从共享队列转移任务后在锁外执行，停止时仍排空已接收任务。
/// @return 队列排空且停止请求成立时返回零。
/// @warning 后台等待可阻塞，任务不得依赖同池耗尽后的嵌套等待。
int ThreadPool::workerLoop()
{
    while ( true ) {
        // 从共享队列转移到线程局部后执行，任务捕获保持到本轮结束。
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(m_queueMutex);

            // 谓词在持锁时检查，应对伪唤醒；休眠期间释放队列锁供生产者入队。
            // 这是后台工作线程的空闲等待，不允许搬入音频拉取调用链。
            this->m_condition.wait(
                lock, [this] { return m_stop || !m_tasks.empty(); });

            // 停止请求不取消已排队任务，只有共享队列已排空才退出。
            if ( m_stop && m_tasks.empty() ) {
                return 0;
            }

            if ( !m_tasks.empty() ) {
                // 队首任务只归一个工作线程；移动后立刻出队，不在锁内运行用户代码。
                task = std::move(m_tasks.front());
                m_tasks.pop();
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
        // 任务捕获也在锁外析构，释放用户资源时不能反向占住共享队列锁。
    }
}

/// @brief 唤醒并等待工作线程，清空已释放句柄以允许重复清理。
/// @warning 构造失败或析构的低频路径；不提供强制取消和等待时限。
void ThreadPool::stopWorkers()
{
    {
        // 停止与取任务共用锁，通知后每个空闲线程都能观察退出条件。
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_stop = true;
    }
    m_condition.notify_all();
    // 等待期间释放队列锁，让已唤醒线程能够取出剩余任务或观察停止条件。
    // 等待会释放 SDL 句柄，清空容器避免析构重复等待已失效的指针。
    for ( SDL_Thread* worker : m_workers ) SDL_WaitThread(worker, nullptr);
    m_workers.clear();
}
}  // namespace ice
