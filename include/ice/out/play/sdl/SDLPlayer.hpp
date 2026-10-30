#pragma once

#include <ice/config/config.hpp>
#include <ice/manage/AudioBuffer.hpp>
#include <ice/manage/AudioFormat.hpp>
#include <ice/out/IReceiver.hpp>

#include <SDL3/SDL_audio.h>

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
/// @brief 按 SDL 音频流需求拉取固定块音频图数据的播放后端。
/// @warning 生命周期由控制侧串行管理；停止会禁用回调并等待在途处理完成。
/// 析构必须在音频回调之外、SDL 后端退出之前完成。
class SDLPlayer : public IReceiver
{
public:
    /// @brief 预分配图处理缓冲，格式在播放期间保持不变。
    explicit SDLPlayer(
        const AudioDataFormat& format = ICEConfig::internal_format);
    /// @brief 停止供数并释放实例拥有的流，不退出全局 SDL 后端。
    /// @warning 低频控制侧析构等待在途回调并回收设备，不能在音频回调执行。
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

    /// @brief 打开 SDL 默认播放设备，尚不启用供数回调。
    bool open() override;

    /// @brief 切换到指定设备，已有流先执行 close。
    /// SDL 打开失败时保留当前线程错误，可立即读取 SDL_GetError。
    /// 构造格式校验失败不调用 SDL，此时其历史错误不代表本次失败原因。
    bool open(SDL_AudioDeviceID deviceid);

    /// @brief 先禁用并等待供数回调，再销毁流及其绑定的设备。
    /// @warning 等待在途图处理和设备释放，只允许低频控制侧调用。
    void close() override;

    /// @brief 注册按需供数回调并恢复设备，返回值不证明后续提交成功。
    bool start() override;

    /// @brief 禁用供数并暂停设备，保留流队列及已生成但未提交的块尾。
    /// @warning 低频控制侧操作，等待在途回调；返回后可安全替换音频图。
    void stop() override;

    /// @brief 返回供数运行请求，暂停时仍为 true，不代表源正在发声。
    /// @warning 高频查询以 relaxed 读取控制侧及回调写入的状态，不访问设备。
    /// 查询不能代替 stop 的回调屏障，源绑定和生命周期必须由控制侧串行管理。
    bool is_running() const override
    {
        return m_running.load(std::memory_order_relaxed);
    }

    /// @brief 暂停设备并禁用回调，保留队列和块尾供恢复时继续。
    /// @warning 低频控制路径等待在途回调，不能在图处理或持有图依赖锁时调用。
    void pause();
    /// @brief 运行中恢复设备，停止后调用不生效。
    void resume();

    /// @brief 返回打开时缓存的设备 ID，不查询热插拔状态。
    inline SDL_AudioDeviceID get_current_device() const
    {
        return m_currentDevice;
    }

private:
    /// @brief 将 SDL 的输入字节需求转交给实例，不使用 totalAmount 预排数据。
    /// @warning 音频热路径：SDL 已持有流锁，不再加锁、分配或复制共享所有权。
    /// m_running 由控制侧写入、回调读取并在失败时清零，使用 relaxed 传递状态；
    /// 缓冲与源的发布、回收由 SDL 回调注册锁和禁用回调屏障保证。
    static void SDLCALL supplyAudio(void* userdata, SDL_AudioStream* stream,
                                    int additionalAmount, int totalAmount);

    /// @brief 按请求提交 PCM，固定块的未消费尾部保留到下一次需求。
    /// @warning 每次设备取数时执行；不得引入等待、堆操作、文件 IO 或日志。
    /// 只以 relaxed 读取运行请求，不在此管理设备生命周期；SDL
    /// 提交沿用流内部同步。
    void supplyAudio(SDL_AudioStream* stream, int additionalAmount);

    /// @brief 校验固定块契约及全部声道指针，拒绝节点破坏的缓冲。
    /// @warning 每块处理前后调用，只检查已有状态，不分配、不加锁。
    bool hasValidBuffer() const;

    /// @brief 图输出格式，提交时按相同声道顺序转为交错 float。
    AudioDataFormat m_playFormat;

    /// @brief 供数回调复用的固定容量平面缓冲，维持原有图处理块大小。
    AudioBuffer m_buffer;

    /// @brief 控制侧首次启动分配的交错存储，回调完整生成一块后分次提交。
    /// 重启复用固定容量，析构在禁用回调后回收，分配失败通过 start 返回。
    std::unique_ptr<float, decltype(&std::free)> m_interleavedBuffer{
        nullptr, &std::free
    };

    /// @brief 构造成功时保存的固定块帧数，零表示缓冲未准备成功。
    /// 仅构造侧写入，重启不能采用源节点修改后的活动长度作为新契约。
    uint32_t m_blockFrames{ 0 };

    /// @brief 当前交错块已提交的帧数，等于 m_blockFrames 表示需要拉取新块。
    /// 仅回调访问；控制侧只在回调禁用后重置，暂停和停止保留未提交样本。
    uint32_t m_pendingFrameOffset{ 0 };

    /// @brief 控制侧运行请求，供数回调在处理或提交失败时清零。
    /// @warning 控制侧和回调写入，状态查询与回调读取；全部使用 relaxed。
    /// 原子仅用于及时取消和错误状态，源与缓冲生命周期依赖禁用回调屏障。
    std::atomic<bool> m_running{ false };
    /// @brief 控制侧记录回调是否仍已注册，故障后须先 stop 再重启。
    bool m_callbackAttached{ false };
    /// @brief 控制侧暂停状态，供数回调不读取此值。
    /// 生命周期串行约束保证该值无并发访问，无需原子操作。
    bool m_paused{ false };

    /// @brief 当前流对应的设备 ID，close 后归零。
    SDL_AudioDeviceID m_currentDevice{ 0 };

    /// @brief 独占 SDL 流的裸句柄，显式 close 负责销毁。
    SDL_AudioStream* m_audioStream{ nullptr };
};
}  // namespace ice
