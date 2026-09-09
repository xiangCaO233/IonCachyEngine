#include <ice/out/play/sdl/SDLPlayer.hpp>

#include <ice/config/config.hpp>
#include <ice/core/IAudioNode.hpp>
#include <ice/manage/AudioBuffer.hpp>
#include <ice/manage/AudioFormat.hpp>

#include <SDL3/SDL_audio.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_thread.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <memory>
#include <thread>
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

/// @brief 分配固定格式的图输出块，设备和线程仍处于未打开状态。
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
    // 在分配前约束三个块的队列阈值，连同单块提交字节数都必须能装入 int。
    // 逐项除法避免先乘声道和帧数产生溢出；零帧也不能形成有效供数块。
    if ( frames == 0 || frames > std::numeric_limits<int>::max() /
                                     sizeof(float) / 3 /
                                     m_playFormat.channels ) {
        return;
    }
    // 仅在缓冲准备成功后记录原始契约，后续启动不能信任节点修改后的元数据。
    if ( m_buffer.resize(m_playFormat, frames) ) m_blockFrames = frames;
}

/// @brief 在成员及基类音频源仍有效时完成实例资源回收。
/// @warning 控制侧低频析构会等待供数线程退出，SDL 后端必须仍有效。
SDLPlayer::~SDLPlayer()
{
    // 先 join 再释放流，避免线程访问随后销毁的缓冲与音频源。
    // close 可重复调用，因此显式提前关闭与析构兜底可共存。
    close();
}

/// @brief 使用 SDL 默认播放设备标识打开流，由 SDL 选择当前默认输出。
bool SDLPlayer::open()
{
    return open(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK);
}

/// @brief 关闭旧流后以指定设备建立浮点输入流。
/// @return 流非空时为 true，此时尚未创建供数线程。
/// @warning 低频控制侧操作，可能停止旧线程并阻塞等待设备打开。
bool SDLPlayer::open(SDL_AudioDeviceID deviceid)
{
    // 关闭旧线程前只能查询不可变的构造结果，不能读取供数节点可修改的缓冲。
    // 设备重开与缓冲校验分离；损坏的活动块仍由 start 拒绝。
    if ( m_blockFrames == 0 ) return false;
    if ( m_audioStream ) {
        // 如果已打开，先关闭
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

/// @brief 等待供数退出后销毁音频流，解除设备绑定。
/// @warning 包含 join 与设备资源回收，不能在供数线程自身或每帧路径调用。
void SDLPlayer::close()
{
    // 先 join 防止后台线程继续读取即将销毁的 m_audioStream。
    stop();

    // 对于由 SDL_OpenAudioDeviceStream 创建的流，
    // 调用 SDL_DestroyAudioStream 会自动关闭设备并处理解绑。
    if ( m_audioStream ) {
        SDL_DestroyAudioStream(m_audioStream);
        m_audioStream = nullptr;
    }
    // 确保设备 ID 被重置
    m_currentDevice = 0;
}

/// @brief 恢复设备并创建供数线程，使用已打开流的现有队列。
/// @return 设备恢复且线程发起成功时为 true，不反映后续提交的错误。
/// @pre 生命周期串行，上一轮线程已 join；源绑定在停止状态完成。
bool SDLPlayer::start()
{
    if ( m_running.load(std::memory_order_relaxed) ||
         m_audioThread != nullptr || !m_audioStream ) {
        // 故障退出也可能留下待 join 的线程，必须先 stop 回收再重新启动。
        return false;
    }
    // 上轮源节点可能破坏块契约；先拒绝异常状态，不能将其用于新线程的分配。
    if ( m_blockFrames == 0 || m_buffer.afmt != m_playFormat ||
         m_buffer.num_frames() != m_blockFrames ||
         m_buffer.frame_capacity() < m_blockFrames ) {
        return false;
    }
    // 上轮节点可能破坏声道观察指针；先拒绝，不能让下一轮 clear
    // 或读取访问空平面。
    auto planes = m_buffer.raw_ptrs();
    if ( !planes ) return false;
    for ( uint16_t ch = 0; ch < m_playFormat.channels; ++ch ) {
        if ( !planes[ch] ) return false;
    }
    // 必须在线程创建和设备恢复前分配，避免后台容器分配失败越过返回值契约。
    // 构造时已验证字节数边界；float 不要求超出 malloc 保证的对齐。
    if ( !m_interleavedBuffer ) {
        const size_t bytes = static_cast<size_t>(m_blockFrames) *
                             m_playFormat.channels * sizeof(float);
        m_interleavedBuffer.reset(static_cast<float*>(std::malloc(bytes)));
        if ( !m_interleavedBuffer ) return false;
    }
    // 恢复失败时保留停止状态与现有流，调用方可重试或 close。
    // 必须先确认设备可恢复，再发布运行请求，避免产生虚假的启动成功。
    if ( !SDL_ResumeAudioDevice(SDL_GetAudioStreamDevice(m_audioStream)) ) {
        return false;
    }
    m_running.store(true, std::memory_order_relaxed);
    m_paused = false;
    // 重启不清空旧流队列，设备恢复后可能先消费上一轮尚未播放的音频。

    // 后台借用 this，实例必须至少存活到 stop 的 join 完成。
    m_audioThread = SDL_CreateThread(runAudioThread, "ICE SDL audio", this);
    // SDL 用空句柄报告创建失败，不能留下没有供数线程的运行请求。
    if ( !m_audioThread ) {
        m_running.store(false, std::memory_order_relaxed);
        return false;
    }

    return true;
}

/// @brief 查询线程是否可 join；已结束但未回收的线程同样返回 true。
bool SDLPlayer::joinable()
{
    return m_audioThread != nullptr;
}

/// @brief 直接等待供数线程结束，不主动发出停止请求。
/// @pre joinable 为 true 且不是当前线程；通常应通过 stop 完成停止与回收。
/// @warning 低频控制侧阻塞等待，运行标志未清除时可能一直等待。
void SDLPlayer::join()
{
    SDL_WaitThread(m_audioThread, nullptr);
    m_audioThread = nullptr;
}

/// @brief 发布退出标志并等待供数线程返回，保留流与设备供再次启动。
/// @warning join 为低频控制侧阻塞操作，不得持有图处理所依赖的锁再调用。
void SDLPlayer::stop()
{
    // 即使后台已因错误清零标志，仍须回收其线程对象，不能提前返回。
    m_running.store(false, std::memory_order_relaxed);

    // 返回后线程不再访问流；暂停设备和排队数据仍由后续 close 或 start 管理。
    join();
}
/// @brief 暂停设备消耗队列，但不暂停上游供数循环。
/// @warning m_paused 仅由串行控制侧读写；后台仍可能填充到队列阈值后再节流。
void SDLPlayer::pause()
{
    if ( m_running.load(std::memory_order_relaxed) && !m_paused ) {
        // 仅确认逻辑设备已暂停后更新标记；失败保留原状态以允许重试。
        if ( SDL_PauseAudioDevice(SDL_GetAudioStreamDevice(m_audioStream)) ) {
            m_paused = true;
        }
    }
}
/// @brief 在运行请求仍有效时取消暂停并恢复设备消耗队列。
/// @warning 暂停状态仅由串行控制侧访问，不能与 open/close 并发。
void SDLPlayer::resume()
{
    if ( m_running.load(std::memory_order_relaxed) && m_paused ) {
        // 恢复失败仍视为暂停，避免后续恢复请求被状态检查直接丢弃。
        if ( SDL_ResumeAudioDevice(SDL_GetAudioStreamDevice(m_audioStream)) ) {
            m_paused = false;
        }
    }
}

/// @brief 适配 SDL 的线程回调签名，不转移播放器所有权。
/// @warning 每次启动执行一次，调用方通过 SDL_WaitThread 等待全部实例访问结束。
int SDLCALL SDLPlayer::runAudioThread(void* userdata)
{
    // 参数由 start 传入非空 this，控制侧在等待完成前不能销毁实例。
    static_cast<SDLPlayer*>(userdata)->audio_thread_loop();
    return 0;
}

/// @brief 按队列输入字节数节流，将音频图平面数据转成交错数据提交。
/// @warning 专用供数线程持续读取 m_running，退出时写回 false 供控制侧查询。
/// 标志使用 relaxed 仅表达请求与状态，线程创建和 join 负责资源同步。
/// 循环包含 SDL 调用与 1ms 音频轮询休眠，不是无阻塞实时回调。
/// 禁止加入文件 IO、逐块分配或诊断输出；源节点也必须满足自身处理约束。
void SDLPlayer::audio_thread_loop()
{
    // 阈值按输入浮点 PCM 的三个块计算，不按设备输出格式或时间戳计量。
    // 判断使用严格大于，恰好三块时还会再送一块，不能将其描述为三块硬上限。
    const auto blockFrames = m_blockFrames;
    const int  target_queued_bytes =
        blockFrames * m_playFormat.channels * sizeof(float) * 3;
    // 控制侧已准备固定容量，线程创建发布存储，join 保证借用结束。
    // 首次提交前逐样本写满，无需依赖分配器清零；契约失败的块不会提交。
    float* const interleavedBuffer = m_interleavedBuffer.get();
    const int blockBytes = blockFrames * m_playFormat.channels * sizeof(float);
    while ( m_running.load(std::memory_order_relaxed) ) {
        // 查询的是尚未转换的输入字节，不能单独代表实际可听设备延迟。
        int queued_bytes = SDL_GetAudioStreamQueued(m_audioStream);
        // 查询失败时不能继续推进图游标，也不在供数线程中反复重试设备错误。
        if ( queued_bytes < 0 ) break;
        // 查询期间可能收到控制侧停止请求，观察到取消后不再推进音频图。
        if ( !m_running.load(std::memory_order_relaxed) ) break;
        // 查询值只是当前快照，设备会并行消费，无需等待队列精确达到某个长度。

        // 只在超过阈值时退让，暂停设备会使队列积累并进入此分支。
        if ( queued_bytes > target_queued_bytes ) {
            if ( !m_running.load(std::memory_order_relaxed) ) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            // 下一轮重新检查退出标志和队列，休眠不承诺精确唤醒时间。
            continue;
        }

        // 源仅在 stop/join 完成后替换，线程创建发布绑定，join 保证借用结束。
        // 直接借用共享指针引用，不产生逐块所有权复制或无配对写锁的读锁。
        m_buffer.clear();
        const auto& source = get_source();
        if ( source ) source->process(m_buffer);
        // 节点返回时重新检查取消，不为已经停止的播放转换或提交当前块。
        if ( !m_running.load(std::memory_order_relaxed) ) break;
        // 清零后的块使空图输出静音，不能把空图当作线程退出条件。
        // 节点必须保持调用方准备的固定块契约；变化后不能沿用交错缓冲容量。
        // 缩短块也必须拒绝，否则整数组提交会把上轮尾部当作新音频播放。
        if ( m_buffer.afmt != m_playFormat ||
             m_buffer.num_frames() != blockFrames ||
             m_buffer.frame_capacity() < blockFrames ) {
            break;
        }
        // 平面到交错转换不需要图锁；声道顺序保持图的现有排列。
        const auto num_frames   = m_buffer.num_frames();
        const auto num_channels = m_playFormat.channels;
        auto       planar_data  = m_buffer.raw_ptrs();

        // 元数据正确不保证节点保留了声道指针；整块检查后才允许任何样本读取。
        // 先检查全部声道，避免前面声道已拷贝而后面空指针触发非法访问。
        bool validPlanes = planar_data != nullptr;
        for ( uint16_t ch = 0; validPlanes && ch < num_channels; ++ch ) {
            validPlanes = planar_data[ch] != nullptr;
        }
        if ( !validPlanes ) break;

        for ( size_t i = 0; i < num_frames; ++i ) {
            // 每帧按声道索引连续排列，字节数随后按整个数组长度统一计算。
            for ( uint16_t ch = 0; ch < num_channels; ++ch ) {
                // 与 OpenAL
                // 浮点路径一致，非有限值按静音处理，避免污染设备转换。
                // 有限样本保留动态范围和符号，不在后端额外限幅或修改图内数据。
                const float sample = planar_data[ch][i];
                interleavedBuffer[i * num_channels + ch] =
                    std::isfinite(sample) ? sample : 0.0f;
            }
        }

        // 转换期间的停止也应在提交前生效；已进入 SDL 的调用仍由 join 等待完成。
        if ( !m_running.load(std::memory_order_relaxed) ) break;
        // SDL 复制提交数据，下一轮可复用本数组；长度单位是字节而非帧。
        // 提交失败立即结束供数；当前块已推进的图游标无法由设备层回滚。
        if ( !SDL_PutAudioStreamData(
                 m_audioStream, interleavedBuffer, blockBytes) ) {
            break;
        }
    }
    // 仅发布停止请求，不用该原子值发布缓冲数据；资源回收仍由控制侧 join 同步。
    m_running.store(false, std::memory_order_relaxed);
}

}  // namespace ice
