#ifndef ICE_SDLPLAYER_HPP
#define ICE_SDLPLAYER_HPP

#include <SDL3/SDL.h>

#include <atomic>
#include <ice/config/config.hpp>
#include <ice/out/IReceiver.hpp>
#include <mutex>
#include <thread>
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
/// @warning 生命周期由控制侧串行管理；类未提供自动 close 的析构实现。
/// 销毁前必须显式 close，确保线程已 join 且裸流句柄已释放。
class SDLPlayer : public IReceiver
{
public:
    /// @brief 预分配图处理缓冲，格式在播放期间保持不变。
    explicit SDLPlayer(
        const AudioDataFormat& format = ICEConfig::internal_format);
    /// @brief 控制侧初始化标志，不代表某个实例或设备已经打开。
    /// @warning 初始化/退出写入与查询使用原子值，但不能代替全局生命周期锁。
    static std::atomic<bool> sdl_inited;
    /// @brief 初始化音频子系统，重复调用按当前实现返回 false。
    static bool init_backend();
    /// @brief 退出整个 SDL，必须先关闭播放器并协调宿主其他 SDL 使用。
    static void quit_backend();

    /// @brief 返回当前设备列表副本，枚举失败时为空。
    static std::vector<SDLAudioDeviceInfo> list_devices();

    /// @brief 打开 SDL 默认播放设备，尚不启动供数线程。
    bool open() override;

    /// @brief 切换到指定设备，已有流先执行 close。
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
    /// @warning 高频查询读取控制侧写入的原子值，不访问设备。
    bool is_running() const override { return running.load(); }

    /// @brief 暂停设备消耗，供数仍可继续至队列超过阈值。
    void pause();
    /// @brief 运行中恢复设备，停止后调用不生效。
    void resume();

    /// @brief start 创建的专用供数线程入口，禁止额外直接调用。
    /// @warning 持续读取控制侧 running 原子请求；现有图锁和 1ms 节流可能阻塞。
    /// 禁止增加文件 IO、逐块内存分配或日志；图绑定只在停止拉取时替换。
    void audio_thread_loop();

    /// @brief 返回打开时缓存的设备 ID，不查询热插拔状态。
    inline SDL_AudioDeviceID get_current_device() const
    {
        return current_device;
    }

private:
    /// @brief 图输出格式，提交时按相同声道顺序转为交错 float。
    AudioDataFormat playformat;

    /// @brief 供数线程复用的固定容量平面缓冲。
    AudioBuffer buffer;

    /// @brief 控制侧启动/停止请求，供数侧每轮检查退出。
    /// @warning 跨线程原子标志保留默认原子序，不保护线程对象或流句柄。
    std::atomic<bool> running{ false };
    /// @brief 控制侧暂停状态，后台供数不读取此值。
    /// @warning pause/resume/stop 读写原子值；状态变更不与整个供数迭代同步。
    std::atomic<bool> paused{ false };

    /// @brief 当前流对应的设备 ID，close 后归零。
    SDL_AudioDeviceID current_device{ 0 };

    /// @brief 独占 SDL 流的裸句柄，显式 close 负责销毁。
    SDL_AudioStream* audio_stream{ nullptr };

    /// @brief 图处理读锁；基类 set_source 不参与该锁，不能保证并发替换安全。
    std::mutex source_mutex;
    /// @brief 借用本实例的供数线程，销毁实例前须停止并 join。
    std::thread sdl_audio_thread;
};
}  // namespace ice

#endif  // ICE_SDLPLAYER_HPP
