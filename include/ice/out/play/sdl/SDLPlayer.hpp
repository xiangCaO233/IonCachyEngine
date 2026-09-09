#pragma once

#include <ice/config/config.hpp>
#include <ice/manage/AudioBuffer.hpp>
#include <ice/manage/AudioFormat.hpp>
#include <ice/out/IReceiver.hpp>

#include <SDL3/SDL_audio.h>
#include <SDL3/SDL_thread.h>

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

namespace ice
{
/// @brief 设备枚举快照，名称为副本，ID 有效性仍受热插拔影响。
struct SDLAudioDeviceInfo {
    /// @brief 人类可读名称，库未返回名称时使用占位文本。
    std::string name;
    /// @brief 用于显式打开的 SDL 设备 ID，不拥有设备资源。
    SDL_AudioDeviceID id;
};
/// @brief 主动推送音频图数据的 SDL 播放后端。
/// @warning 生命周期由控制侧串行管理；析构自动停止线程并关闭音频流。
/// 析构必须在供数线程之外、SDL 后端退出之前完成。
class SDLPlayer : public IReceiver
{
public:
    /// @brief 预分配图处理缓冲，格式在播放期间保持不变。
    explicit SDLPlayer(
        const AudioDataFormat& format = ICEConfig::internal_format);
    /// @brief 停止供数并释放实例拥有的流，不退出全局 SDL 后端。
    /// @warning 低频控制侧析构包含 join 与设备回收，不能在供数线程执行。
    ~SDLPlayer() override;

    /// @brief 控制侧初始化标志，不代表某个实例或设备已经打开。
    /// @warning 初始化/退出写入与查询使用原子值，但不能代替全局生命周期锁。
    static std::atomic<bool> sdl_inited;
    /// @brief 主线程初始化音频子系统，重复调用按当前实现返回 false。
    /// SDL 初始化失败时可在同一线程立即读取
    /// SDL_GetError；重复调用不设置新错误。
    static bool init_backend();
    /// @brief 释放本后端音频子系统引用，必须先关闭播放器。
    /// @warning 宿主须串行管理初始化与退出，并在全部 SDL 使用结束后执行
    /// SDL_Quit。
    static void quit_backend();

    /// @brief 返回当前设备列表副本，枚举失败时为空。
    static std::vector<SDLAudioDeviceInfo> list_devices();

    /// @brief 打开 SDL 默认播放设备，尚不启动供数线程。
    bool open() override;

    /// @brief 切换到指定设备，已有流先执行 close。
    /// SDL 打开失败时保留当前线程错误，可立即读取 SDL_GetError。
    /// 构造格式校验失败不调用 SDL，此时其历史错误不代表本次失败原因。
    bool open(SDL_AudioDeviceID deviceid);

    /// @brief 先结束供数线程，再销毁流及其绑定的设备。
    /// @warning 包含 join 和设备释放，只允许低频控制侧调用。
    void close() override;

    /// @brief 恢复设备并发起供数线程，返回值不证明后续提交成功。
    bool start() override;

    /// @brief 查询线程是否仍需 join，不等同于设备正在播放。
    bool joinable();

    /// @brief 等待线程结束，不主动停止供数；须先保证线程可 join。
    /// @warning 低频控制侧阻塞操作，不能由供数线程自行调用。
    void join();

    /// @brief 发布退出请求并 join，保留音频流及队列。
    /// @warning 低频控制侧操作，等待当前图处理及 SDL 调用结束。
    void stop() override;

    /// @brief 返回供数运行请求，不等于源播放或线程回收状态。
    /// @warning 高频查询以 relaxed 读取控制侧和供数侧写入的状态，不访问设备。
    /// 查询不能代替 join，源绑定和生命周期必须由控制侧串行管理。
    bool is_running() const override
    {
        return m_running.load(std::memory_order_relaxed);
    }

    /// @brief 暂停设备消耗，供数仍可继续至队列超过阈值。
    void pause();
    /// @brief 运行中恢复设备，停止后调用不生效。
    void resume();

    /// @brief start 创建的专用供数线程入口，禁止额外直接调用。
    /// @warning 持续以 relaxed 读取控制侧 m_running 请求，退出时写回 false。
    /// 线程创建和 join 同步资源；现有 1ms 音频节流可能阻塞。
    /// 禁止增加文件 IO、逐块内存分配或日志；图绑定只在停止拉取时替换。
    void audio_thread_loop();

    /// @brief 返回打开时缓存的设备 ID，不查询热插拔状态。
    inline SDL_AudioDeviceID get_current_device() const
    {
        return m_currentDevice;
    }

private:
    /// @brief 将 SDL 线程入口的借用参数转交给实例供数循环。
    /// @warning 仅由 SDL 创建的线程调用，实例存活到控制侧等待线程结束。
    static int SDLCALL runAudioThread(void* userdata);

    /// @brief 图输出格式，提交时按相同声道顺序转为交错 float。
    AudioDataFormat m_playFormat;

    /// @brief 供数线程复用的固定容量平面缓冲。
    AudioBuffer m_buffer;

    /// @brief 控制侧首次启动分配的交错存储，供数线程仅借用并完整写入。
    /// 重启复用固定容量，析构在 join 后由 free 回收，分配失败通过 start 返回。
    std::unique_ptr<float, decltype(&std::free)> m_interleavedBuffer{
        nullptr, &std::free
    };

    /// @brief 构造成功时保存的固定块帧数，零表示缓冲未准备成功。
    /// 仅构造侧写入，重启不能采用源节点修改后的活动长度作为新契约。
    uint32_t m_blockFrames{ 0 };

    /// @brief 控制侧启动/停止请求，供数侧每轮检查并在退出时清零。
    /// @warning
    /// 控制侧写请求、供数侧读请求并写退出状态；原子值不保护流或线程对象。
    /// 全部访问使用 relaxed，仅表达请求与状态，资源发布与回收依赖线程创建和
    /// join。
    std::atomic<bool> m_running{ false };
    /// @brief 控制侧暂停状态，后台供数不读取此值。
    /// 生命周期串行约束保证该值无并发访问，无需原子操作。
    bool m_paused{ false };

    /// @brief 当前流对应的设备 ID，close 后归零。
    SDL_AudioDeviceID m_currentDevice{ 0 };

    /// @brief 独占 SDL 流的裸句柄，显式 close 负责销毁。
    SDL_AudioStream* m_audioStream{ nullptr };

    /// @brief 借用本实例的供数线程，销毁实例前须停止并 join。
    SDL_Thread* m_audioThread{ nullptr };
};
}  // namespace ice
