#include <ice/out/play/sdl/SDLPlayer.hpp>

#include <ice/config/config.hpp>
#include <ice/core/IAudioNode.hpp>
#include <ice/manage/AudioBuffer.hpp>
#include <ice/manage/AudioFormat.hpp>

#include <SDL3/SDL_audio.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_stdinc.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <memory>
#include <vector>

namespace ice
{
/// @brief 全局初始化标志，由控制侧串行初始化与退出流程更新。
std::atomic<bool> SDLPlayer::sdl_inited{ false };
/// @brief 初始化 SDL 音频子系统，重复初始化按当前接口返回 false。
/// @warning 涉及系统设备初始化，只允许主线程低频调用。
/// 原子标志并非一次性初始化锁，调用方必须串行管理全局生命周期。
bool SDLPlayer::init_backend()
{
    if ( !sdl_inited.load() ) {
        // 仅在库确认初始化成功后发布标志，失败时允许后续重新尝试。
        if ( SDL_Init(SDL_INIT_AUDIO) ) {
            sdl_inited.store(true);
            return true;
        } else {
            // 保留 SDL 当前线程的错误，交由调用方在其他 SDL 调用前读取。
            return false;
        }
    } else {
        // 重复初始化没有取得新引用，也不生成或打印新的 SDL 错误。
        return false;
    }
}

/// @brief 结束 SDL 音频后端全局状态。
/// @pre 所有播放器已 close；宿主串行管理 SDL 生命周期。
/// @warning 低频子系统回收不是线程安全操作，不能与其他初始化或退出并发。
void SDLPlayer::quit_backend()
{
    if ( sdl_inited.load() ) {
        // 只释放本后端成功初始化取得的音频引用，不强制关闭宿主其他子系统。
        // SDL 全局收尾由宿主在全部使用者结束后统一执行。
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        sdl_inited.store(false);
    }
}

/// @brief 复制当前播放设备名和 ID，返回值不借用 SDL 的枚举数组。
/// @warning 低频设备管理操作，会分配并访问设备接口。
std::vector<SDLAudioDeviceInfo> SDLPlayer::list_devices()
{
    std::vector<SDLAudioDeviceInfo> devices;
    int                             count = 0;
    if ( auto sdl_devices = SDL_GetAudioPlaybackDevices(&count) ) {
        // 立即接管枚举数组，名称复制或结果容器扩容发生栈展开时也必须释放。
        // 守卫仅拥有 SDL 数组；返回值中的名称与 ID 始终独立保存。
        const std::unique_ptr<SDL_AudioDeviceID, decltype(&SDL_free)> owner(
            sdl_devices, &SDL_free);
        // 列表是枚举时刻快照，热插拔后仍须检查 open 结果，不能仅凭 ID
        // 判定可用。
        for ( int i = 0; i < count; ++i ) {
            SDLAudioDeviceInfo info;
            info.id          = sdl_devices[i];
            const char* name = SDL_GetAudioDeviceName(sdl_devices[i]);
            info.name        = name ? name : "Unknown Device";
            devices.push_back(info);
        }
        // 即使成功取得的是空列表，离开此作用域也由守卫释放数组。
    }

    return devices;
}

/// @brief 分配固定格式的图输出块，设备和回调仍处于未打开状态。
/// @param format 有效的目标浮点音频格式，播放期间不得改变块容量与声道数。
SDLPlayer::SDLPlayer(const AudioDataFormat& format)
    : IReceiver(format), m_playFormat(format)
{
    // SDL 使用有符号采样率；非法格式保留空缓冲，由 open 返回失败。
    if ( m_playFormat.channels == 0 || m_playFormat.samplerate == 0 ||
         m_playFormat.samplerate > std::numeric_limits<int>::max() ) {
        return;
    }
    const auto frames = ICEConfig::default_buffer_size;
    // 单块提交字节数必须能装入 SDL 接口使用的 int，不再预留多块队列阈值。
    // 逐项除法避免先乘声道和帧数产生溢出；零帧也不能形成有效供数块。
    if ( frames == 0 || frames > std::numeric_limits<int>::max() /
                                     sizeof(float) / m_playFormat.channels ) {
        return;
    }
    // 仅在缓冲准备成功后记录原始契约，后续启动不能信任节点修改后的元数据。
    if ( m_buffer.resize(m_playFormat, frames) ) {
        m_blockFrames        = frames;
        m_pendingFrameOffset = frames;
    }
}

/// @brief 在成员及基类音频源仍有效时完成实例资源回收。
/// @warning 控制侧低频析构会等待在途供数回调，SDL 后端必须仍有效。
SDLPlayer::~SDLPlayer()
{
    // 先禁用并等待回调，再释放流、缓冲与音频源。
    // close 可重复调用，因此显式提前关闭与析构兜底可共存。
    close();
}

/// @brief 使用 SDL 默认播放设备标识打开流，由 SDL 选择当前默认输出。
bool SDLPlayer::open()
{
    return open(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK);
}

/// @brief 关闭旧流后以指定设备建立浮点输入流。
/// @return 流非空时为 true，此时设备暂停且尚未注册供数回调。
/// @warning 低频控制侧操作，可能等待旧回调及设备打开，不得在音频回调调用。
bool SDLPlayer::open(SDL_AudioDeviceID deviceid)
{
    // 禁用旧回调前只能查询不可变的构造结果，不能读取节点可修改的缓冲。
    // 设备重开与缓冲校验分离；损坏的活动块仍由 start 拒绝。
    if ( m_blockFrames == 0 ) return false;
    if ( m_audioStream ) {
        // 切换设备前解除旧回调对实例的借用。
        close();
    }

    // 指定本端输入格式，SDL 负责与设备实际格式衔接，不修改音频图格式。
    SDL_AudioSpec source_spec = { SDL_AUDIO_F32,
                                  m_playFormat.channels,
                                  static_cast<int>(m_playFormat.samplerate) };

    m_audioStream =
        SDL_OpenAudioDeviceStream(deviceid, &source_spec, nullptr, nullptr);

    if ( m_audioStream == nullptr ) {
        // 失败后保留原始线程错误，不能再用空流查询设备并覆盖诊断。
        m_currentDevice = 0;
        return false;
    }

    // 仅成功创建的流可用于查询，缓存 ID 与本次打开结果保持一致。
    m_currentDevice = SDL_GetAudioStreamDevice(m_audioStream);
    return true;
}

/// @brief 等待供数回调退出后销毁音频流，解除设备绑定。
/// @warning 包含回调屏障与设备回收，不能在音频回调自身或每帧路径调用。
void SDLPlayer::close()
{
    // 禁用回调会等待在途处理，返回后才可销毁其借用的流与实例成员。
    stop();

    // 对于由 SDL_OpenAudioDeviceStream 创建的流，
    // 调用 SDL_DestroyAudioStream 会自动关闭设备并处理解绑。
    if ( m_audioStream ) {
        SDL_DestroyAudioStream(m_audioStream);
        m_audioStream = nullptr;
    }
    // 新流不能接收旧设备的块尾；普通 stop/start 则保留同一流的样本顺序。
    m_pendingFrameOffset = m_blockFrames;
    m_currentDevice      = 0;
}

/// @brief 预备交错存储并注册按需回调，随后恢复设备。
/// @return 注册和设备恢复成功时为 true，不反映后续图处理或提交的错误。
/// @pre 生命周期串行，源绑定在 stop 返回之后、start 之前完成。
bool SDLPlayer::start()
{
    if ( m_running.load(std::memory_order_relaxed) || m_callbackAttached ||
         !m_audioStream ) {
        // 故障只清除运行状态，须先 stop 完成回调屏障再检查和复用缓冲。
        return false;
    }
    // 失败节点可能破坏格式、活动帧数或平面指针，不能把损坏缓冲交给回调。
    if ( !hasValidBuffer() ) return false;
    // 必须在注册回调和恢复设备前分配，回调仅复用已有存储。
    // 构造时已验证字节数边界；float 不要求超出 malloc 保证的对齐。
    if ( !m_interleavedBuffer ) {
        const size_t bytes = static_cast<size_t>(m_blockFrames) *
                             m_playFormat.channels * sizeof(float);
        m_interleavedBuffer.reset(static_cast<float*>(std::malloc(bytes)));
        if ( !m_interleavedBuffer ) return false;
    }
    // 回调注册的流锁发布已完成的源绑定与存储准备；此时设备仍处于暂停。
    if ( !SDL_SetAudioStreamGetCallback(m_audioStream, supplyAudio, this) ) {
        return false;
    }
    m_callbackAttached = true;
    // 在恢复设备前允许供数，避免首个设备请求只能获得静音。
    m_running.store(true, std::memory_order_relaxed);
    if ( !SDL_ResumeAudioDevice(m_currentDevice) ) {
        // 失败时撤销借用；不销毁流，允许控制侧再次启动或关闭。
        m_running.store(false, std::memory_order_relaxed);
        SDL_SetAudioStreamGetCallback(m_audioStream, nullptr, nullptr);
        m_callbackAttached = false;
        return false;
    }
    m_paused = false;
    return true;
}

/// @brief 禁用回调并暂停设备，保留同一流已有队列与未提交的块尾。
/// @details 禁用回调是源绑定的生命周期屏障，与运行标志的观察值职责不同。
/// 即使设备恢复失败或回调已报告错误，仍须经过屏障才允许释放音频图。
/// @warning 低频控制路径等待在途图处理，不得在回调或持有图依赖锁时调用。
void SDLPlayer::stop()
{
    // 即使回调已经因错误清零，仍须解除注册并等待其返回，不能提前结束。
    m_running.store(false, std::memory_order_relaxed);
    if ( m_audioStream ) {
        // SDL setter 获取流锁，等待在途回调；未持有流锁时才调用设备接口，
        // 避免与 SDL 音频线程的设备锁、流锁顺序相反。
        SDL_SetAudioStreamGetCallback(m_audioStream, nullptr, nullptr);
        m_callbackAttached = false;
        SDL_PauseAudioDevice(m_currentDevice);
    }
    m_paused = false;
}
/// @brief 暂停设备并等待回调结束，不再提前推进上游音频图。
/// @details 保留运行意图，使 resume 可以恢复；停止后的 resume 不会隐式重启。
/// 不清空流队列和本端块尾，避免暂停导致音频帧被重复消费或丢失。
/// @warning 低频控制路径包含设备同步和回调屏障，不能在音频回调调用。
void SDLPlayer::pause()
{
    if ( m_running.load(std::memory_order_relaxed) && !m_paused ) {
        // 仅确认逻辑设备已暂停后更新标记；失败保留原状态以允许重试。
        if ( SDL_PauseAudioDevice(m_currentDevice) ) {
            SDL_SetAudioStreamGetCallback(m_audioStream, nullptr, nullptr);
            m_callbackAttached = false;
            m_paused           = true;
        }
    }
}
/// @brief 在运行请求仍有效时取消暂停并恢复设备消耗队列。
/// @details 注册回调与恢复设备分开完成，恢复失败时撤销注册但保留暂停状态。
/// @warning 暂停状态仅由串行控制侧访问，不能与 open/close 并发。
void SDLPlayer::resume()
{
    if ( m_running.load(std::memory_order_relaxed) && m_paused ) {
        // 先重新注册以发布暂停期间的状态；不丢弃队列和固定块的剩余样本。
        if ( !SDL_SetAudioStreamGetCallback(m_audioStream, supplyAudio, this) )
            return;
        m_callbackAttached = true;
        if ( SDL_ResumeAudioDevice(m_currentDevice) ) {
            m_paused = false;
        } else {
            // 恢复失败仍保持暂停，并解除回调借用以便重试或关闭。
            SDL_SetAudioStreamGetCallback(m_audioStream, nullptr, nullptr);
            m_callbackAttached = false;
        }
    }
}

/// @brief 适配 SDL 的按需回调，只处理尚未排队的输入字节需求。
/// @param userdata 生命周期覆盖回调注册区间的播放器借用地址。
/// @param stream 当前请求数据的已注册流，不在回调内改变其绑定。
/// @param additionalAmount 当前还缺少的输入字节数，零需求不推进音频图。
/// @warning 每次音频取数时执行；SDL 已持有流锁，不再添加锁、等待或堆操作。
/// relaxed 运行标志只表达取消和错误，资源生命周期由注册及禁用回调屏障保证。
void SDLCALL SDLPlayer::supplyAudio(void* userdata, SDL_AudioStream* stream,
                                    int additionalAmount, int)
{
    // totalAmount 包含流内已有样本，使用它供数会重新引入无必要的预排。
    static_cast<SDLPlayer*>(userdata)->supplyAudio(stream, additionalAmount);
}

/// @brief 确认固定块及所有平面仍满足构造时的存储契约。
/// @return 格式、活动长度、容量和各声道地址均有效时返回 true。
/// @details 同时用于启动失败返回值和自定义节点处理后的边界防护。
/// @warning 每音频块处理前后执行；仅访问已有元数据和指针，不分配、不加锁。
bool SDLPlayer::hasValidBuffer() const
{
    if ( m_blockFrames == 0 || m_buffer.afmt != m_playFormat ||
         m_buffer.num_frames() != m_blockFrames ||
         m_buffer.frame_capacity() < m_blockFrames )
        return false;
    // 声道表也可能被自定义节点破坏，必须在 clear 和采样访问之前完整校验。
    const auto planes = m_buffer.raw_ptrs();
    if ( !planes ) return false;
    for ( uint16_t channel = 0; channel < m_playFormat.channels; ++channel ) {
        if ( !planes[channel] ) return false;
    }
    return true;
}

/// @brief 固定块处理音频图，分段提交当前请求需要的交错 PCM。
/// @param stream 本次设备需求对应的流，提交量按其输入格式计算。
/// @param additionalAmount 当前缺少的字节数，不包含流内已缓存的数据。
/// @details 图游标最多领先已提交数据一个固定块，未提交部分归本端持有。
/// 本地块尾与 SDL 队列分离，既保持固定块处理，也避免提前拉取多块。
/// @warning 音频热路径无自建线程、轮询、睡眠和共享所有权复制。
/// 每块读取控制侧 relaxed
/// 运行请求，失败只清零状态，不在回调里管理设备或输出日志。 SDL
/// 提交仍使用其内部流同步和缓存，不能据此宣称整个 SDL 调用链零分配。
void SDLPlayer::supplyAudio(SDL_AudioStream* stream, int additionalAmount)
{
    if ( additionalAmount <= 0 ) return;
    const std::size_t frameBytes = m_playFormat.channels * sizeof(float);
    // SDL 的字节需求可能因重采样保守估计而不是整帧；向上取整不足一帧的部分。
    // 使用 size_t 计算，避免接近 int 上限的请求在取整时发生有符号溢出。
    std::size_t remainingFrames =
        (static_cast<std::size_t>(additionalAmount) + frameBytes - 1) /
        frameBytes;
    float* const interleavedBuffer = m_interleavedBuffer.get();
    while ( remainingFrames > 0 && m_running.load(std::memory_order_relaxed) ) {
        if ( m_pendingFrameOffset == m_blockFrames ) {
            // 保持 ICE
            // 原有固定块调用，不让设备回调长度改变效果器和调度的块契约。
            if ( !hasValidBuffer() ) {
                m_running.store(false, std::memory_order_relaxed);
                return;
            }
            m_buffer.clear();
            const auto& source = get_source();
            if ( source ) source->process(m_buffer);
            // 即使 stop
            // 已提出取消，也保存本块生成结果；否则重启会跳过已推进的样本。
            // 禁用回调等待本次转换结束，随后才能释放源及缓冲。
            if ( !hasValidBuffer() ) {
                m_running.store(false, std::memory_order_relaxed);
                return;
            }
            const auto planes = m_buffer.raw_ptrs();
            for ( std::size_t frame = 0; frame < m_blockFrames; ++frame ) {
                for ( uint16_t channel = 0; channel < m_playFormat.channels;
                      ++channel ) {
                    // 延续原后端约定：非有限样本静音，有限值不额外限幅。
                    const float sample = planes[channel][frame];
                    interleavedBuffer[frame * m_playFormat.channels + channel] =
                        std::isfinite(sample) ? sample : 0.0f;
                }
            }
            m_pendingFrameOffset = 0;
        }
        // 不足整块的请求仅提交前缀，剩余样本保存在固定存储中，不能丢弃或重复。
        const auto frames = std::min(
            remainingFrames,
            static_cast<std::size_t>(m_blockFrames - m_pendingFrameOffset));
        const auto* samples =
            interleavedBuffer + static_cast<std::size_t>(m_pendingFrameOffset) *
                                    m_playFormat.channels;
        if ( !m_running.load(std::memory_order_relaxed) ) return;
        // 构造阶段已约束单块字节数；SDL 复制后即可复用已消费的存储。
        if ( !SDL_PutAudioStreamData(
                 stream, samples, static_cast<int>(frames * frameBytes)) ) {
            m_running.store(false, std::memory_order_relaxed);
            return;
        }
        m_pendingFrameOffset += static_cast<uint32_t>(frames);
        remainingFrames -= frames;
    }
}

}  // namespace ice
