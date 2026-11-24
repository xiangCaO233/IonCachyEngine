#include "ice/core/IAudioNode.hpp"
#include "ice/manage/AudioBuffer.hpp"
#include "ice/out/play/openal/ALPlayer.hpp"

#ifdef _WIN32
// 原生线程优先级仅由 Windows 测试直接读取，不暴露给普通引擎消费者。
#    include <windows.h>
#endif

#include <chrono>
#include <cstdlib>
#include <memory>
#include <semaphore>
#include <string_view>

namespace
{
/// @brief 首次供数时记录当前工作线程状态，不需要真实声卡。
class PlaybackProbe final : public ice::IAudioNode
{
public:
    /// @brief 借用播放缓冲输出静音，并在第一次处理时发布线程探测结果。
    /// @param buffer 调用者准备的完整可写块。
    /// @warning 测试节点在音频供数路径执行，不分配或等待。
    void process(ice::AudioBuffer& buffer) override
    {
        buffer.clear();
        // 供数线程独占此标志，防止后续块重复释放二值信号量。
        if ( m_observed ) return;
        m_observed = true;
#ifdef _WIN32
        // 由真实供数线程读回原生优先级，控制线程的默认值不能满足此断言。
        m_threadConfigured = GetThreadPriority(GetCurrentThread()) ==
                             THREAD_PRIORITY_ABOVE_NORMAL;
#else
        // POSIX 保持原系统调度，只要求设备确实启动了供数。
        m_threadConfigured = true;
#endif
        // 控制侧 acquire 后可以安全读取供数线程发布的普通值。
        m_callback.release();
    }

    /// @brief 等待测试回调已执行，确认结果由供数线程产生。
    /// @return 回调按时执行且观察到该平台预期状态时返回 true。
    bool waitForCallback()
    {
        // 只为测试判断失败设置超时，不影响播放线程如何推进或停止。
        return m_callback.try_acquire_for(std::chrono::seconds(5)) &&
               m_threadConfigured;
    }

private:
    /// @brief 供数线程只在第一次 process 中写入。
    bool m_observed{ false };
    /// @brief 由供数线程先写，控制线程在信号量 acquire 后读。
    bool m_threadConfigured{ false };
    /// @brief 发布第一次回调的状态，避免测试依赖固定延时。
    std::binary_semaphore m_callback{ 0 };
};

/// @brief 检查初始化后的环境值仍符合 CTest 传入的场景。
/// @param name 进程环境变量。
/// @param expected 期望字符串，已存在的非空值应原样保留。
bool envIs(const char* name, std::string_view expected)
{
    const char* value = std::getenv(name);
    return value && value == expected;
}
}  // namespace

/// @brief 在独立进程验证空环境默认、已有配置与空设备供数。
/// @param argc 参数数量，第二项为预期的日志级别。
/// @param argv 由 CTest 指定预期的 OpenAL Soft 日志级别。
/// @return 非零状态标识未通过的阶段。
int main(int argc, char** argv)
{
    if ( argc != 2 || !ice::ALPlayer::init_backend() ) return 1;
    // CTest 显式使用 null 驱动，仅要求 OpenAL 实际创建上下文并启动线程。
    if ( !envIs("ALSOFT_LOGLEVEL", argv[1]) ||
         !envIs("ALSOFT_DRIVERS", "null") )
        return 2;
    // 图节点由控制侧预先分配，真正的供数线程只发布观测值。
    ice::ALPlayer player;
    auto          probe = std::make_shared<PlaybackProbe>();
    player.set_source(probe);
    // 只有已初始化设备才能启动回调，失败由对象析构统一回收。
    if ( !player.open() || !player.start() ) return 3;
    const bool configured = probe->waitForCallback();
    // 先停并等待供数后再清全局初始化标记，回调不借用已失效资源。
    player.close();
    ice::ALPlayer::quit_backend();
    return configured ? 0 : 4;
}
