#include <SDL3/SDL_audio.h>
#include <SDL3/SDL_error.h>
#include <SDL3/SDL_init.h>
#include <fmt/base.h>

#include <ice/config/config.hpp>
#include <ice/manage/AudioBuffer.hpp>
#include <ice/out/play/sdl/SDLPlayer.hpp>

namespace ice
{
/// @brief 全局初始化标志，由控制侧串行初始化与退出流程更新。
std::atomic<bool> SDLPlayer::sdl_inited{ false };
/// @brief 初始化 SDL 音频子系统，重复初始化按当前接口返回 false。
/// @warning 涉及系统设备与诊断输出，只允许低频控制侧调用。
/// 原子标志并非一次性初始化锁，调用方必须串行管理全局生命周期。
bool SDLPlayer::init_backend()
{
    if ( !sdl_inited.load() ) {
        // 仅在库确认初始化成功后发布标志，失败时允许后续重新尝试。
        if ( SDL_Init(SDL_INIT_AUDIO) ) {
            sdl_inited.store(true);
            return true;
        } else {
            fmt::print("init sdl audio failed:{}\n", SDL_GetError());
            return false;
        }
    } else {
        fmt::print("sdl audio already initialized\n");
        return false;
    }
}

/// @brief 结束 SDL 音频后端全局状态。
/// @pre 所有播放器已 close，宿主已协调其余 SDL 子系统的退出顺序。
void SDLPlayer::quit_backend()
{
    if ( sdl_inited.load() ) {
        // SDL_Quit 退出整个 SDL，而非仅音频；此接口不能与宿主其他 SDL
        // 使用并行。
        SDL_Quit();
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
        // 列表是枚举时刻快照，热插拔后仍须检查 open 结果，不能仅凭 ID
        // 判定可用。
        for ( int i = 0; i < count; ++i ) {
            SDLAudioDeviceInfo info;
            info.id          = sdl_devices[i];
            const char* name = SDL_GetAudioDeviceName(sdl_devices[i]);
            info.name        = name ? name : "Unknown Device";
            devices.push_back(info);
        }
        SDL_free(sdl_devices);
        // 枚举结果数组由 SDL 分配，复制完才能释放；名称副本由返回列表持有。
    }

    return devices;
}

/// @brief 分配固定格式的图输出块，设备和线程仍处于未打开状态。
/// @param format 有效的目标浮点音频格式，播放期间不得改变块容量与声道数。
SDLPlayer::SDLPlayer(const AudioDataFormat& format)
    : IReceiver(format), playformat(format)
{
    buffer.resize(playformat, ICEConfig::default_buffer_size);
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
    if ( audio_stream ) {
        // 如果已打开，先关闭
        close();
    }

    // 指定本端输入格式，SDL 负责与设备实际格式衔接，不修改音频图格式。
    SDL_AudioSpec source_spec = { SDL_AUDIO_F32,
                                  playformat.channels,
                                  (int)playformat.samplerate };

    audio_stream =
        SDL_OpenAudioDeviceStream(deviceid, &source_spec, nullptr, nullptr);

    // 当前顺序先查询设备后判断空流；失败诊断可能被此查询的错误状态覆盖。
    current_device = SDL_GetAudioStreamDevice(audio_stream);

    if ( audio_stream == nullptr ) {
        fmt::print("Failed to open audio device stream: {}\n", SDL_GetError());
        return false;
    }

    return true;
}

/// @brief 等待供数退出后销毁音频流，解除设备绑定。
/// @warning 包含 join 与设备资源回收，不能在供数线程自身或每帧路径调用。
void SDLPlayer::close()
{
    // 先 join 防止后台线程继续读取即将销毁的 audio_stream。
    stop();

    // 对于由 SDL_OpenAudioDeviceStream 创建的流，
    // 调用 SDL_DestroyAudioStream 会自动关闭设备并处理解绑。
    if ( audio_stream ) {
        SDL_DestroyAudioStream(audio_stream);
        audio_stream = nullptr;
    }
    // 确保设备 ID 被重置
    current_device = 0;
}

/// @brief 恢复设备并创建供数线程，使用已打开流的现有队列。
/// @return 线程发起成功时为 true，不反映恢复设备或后续提交的错误。
/// @pre 生命周期串行，上一轮线程已 join；源绑定在停止状态完成。
bool SDLPlayer::start()
{
    if ( running.load() || !audio_stream ) {
        // 禁止覆盖运行中的线程对象，也不由后台线程懒创建输入流。
        return false;
    }
    running.store(true);
    paused.store(false);
    // 重启不清空旧流队列，设备恢复后可能先消费上一轮尚未播放的音频。

    // 恢复设备，以防之前被暂停过
    SDL_ResumeAudioDevice(SDL_GetAudioStreamDevice(audio_stream));

    // 后台借用 this，实例必须至少存活到 stop 的 join 完成。
    sdl_audio_thread = std::thread(&SDLPlayer::audio_thread_loop, this);

    return true;
}

/// @brief 查询线程是否可 join；已结束但未回收的线程同样返回 true。
bool SDLPlayer::joinable()
{
    return sdl_audio_thread.joinable();
}

/// @brief 直接等待供数线程结束，不主动发出停止请求。
/// @pre joinable 为 true 且不是当前线程；通常应通过 stop 完成停止与回收。
/// @warning 低频控制侧阻塞等待，运行标志未清除时可能一直等待。
void SDLPlayer::join()
{
    sdl_audio_thread.join();
}

/// @brief 发布退出标志并等待供数线程返回，保留流与设备供再次启动。
/// @warning join 为低频控制侧阻塞操作，不得持有图处理所依赖的锁再调用。
void SDLPlayer::stop()
{
    if ( !running.load() ) {
        // 正常生命周期要求前一次 stop 已回收线程，不能据此分支替代析构保护。
        return;
    }

    // 供数循环和节流分支读取退出标志，当前图处理完成后才会退出。
    running.store(false);

    // running 已清零，resume 的前置条件不成立，此处不会实际恢复设备。
    // 后台采用主动供数轮询而非等待回调，退出依赖 running，不依赖该恢复调用。
    if ( paused.load() ) {
        resume();
    }

    // 返回后线程不再访问流；暂停设备和排队数据仍由后续 close 或 start 管理。
    if ( sdl_audio_thread.joinable() ) {
        sdl_audio_thread.join();
    }
}
/// @brief 暂停设备消耗队列，但不暂停上游供数循环。
/// @warning 控制侧写 paused 原子标记；后台仍可能填充到队列阈值后再节流。
void SDLPlayer::pause()
{
    if ( running.load() && !paused.load() ) {
        paused.store(true);
        // 直接使用 SDL 的函数来暂停物理设备
        SDL_PauseAudioDevice(SDL_GetAudioStreamDevice(audio_stream));
    }
}
/// @brief 在运行请求仍有效时取消暂停并恢复设备消耗队列。
/// @warning 控制侧原子标记不保护流生命周期，不能与 open/close 并发。
void SDLPlayer::resume()
{
    if ( running.load() && paused.load() ) {
        paused.store(false);
        // 恢复物理设备
        SDL_ResumeAudioDevice(SDL_GetAudioStreamDevice(audio_stream));
    }
}

/// @brief 按队列输入字节数节流，将音频图平面数据转成交错数据提交。
/// @warning 专用供数线程持续运行，读取控制侧 running 原子值决定退出。
/// 循环包含图互斥锁、SDL 调用与 1ms 音频轮询休眠，不是无阻塞实时回调。
/// 禁止加入文件 IO、逐块分配或诊断输出；源节点也必须满足自身处理约束。
void SDLPlayer::audio_thread_loop()
{
    // 阈值按输入浮点 PCM 的三个块计算，不按设备输出格式或时间戳计量。
    // 判断使用严格大于，恰好三块时还会再送一块，不能将其描述为三块硬上限。
    const int target_queued_bytes =
        buffer.num_frames() * playformat.channels * sizeof(float) * 3;
    // 交错存储在线程启动时分配一次；循环依赖图处理不改变固定块容量。
    std::vector<float> interleaved_buffer(buffer.num_frames() *
                                          playformat.channels);
    while ( running.load() ) {
        // 查询的是尚未转换的输入字节，不能单独代表实际可听设备延迟。
        int queued_bytes = SDL_GetAudioStreamQueued(audio_stream);
        // 查询值只是当前快照，设备会并行消费，无需等待队列精确达到某个长度。

        // 只在超过阈值时退让，暂停设备会使队列积累并进入此分支。
        if ( queued_bytes > target_queued_bytes ) {
            if ( !running.load() ) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            // 下一轮重新检查退出标志和队列，休眠不承诺精确唤醒时间。
            continue;
        }

        // 返回负队列值也会进入既有供数路径，当前循环没有独立错误重试策略。
        {
            std::scoped_lock<std::mutex> lock(source_mutex);
            // 基类 set_source 不获取此锁，运行中不能据此安全替换共享图指针。
            buffer.clear();
            if ( get_source() ) {
                // 从连接的音频图中拉取标准格式的数据
                get_source()->process(buffer);
            }
            // 清零后的块使空图输出静音，不能把空图当作线程退出条件。
        }
        // 平面到交错转换不需要图锁；声道顺序保持图的现有排列。
        const auto num_frames   = buffer.num_frames();
        const auto num_channels = playformat.channels;
        auto       planar_data  = buffer.raw_ptrs();

        for ( size_t i = 0; i < num_frames; ++i ) {
            // 每帧按声道索引连续排列，字节数随后按整个数组长度统一计算。
            for ( uint16_t ch = 0; ch < num_channels; ++ch ) {
                interleaved_buffer[i * num_channels + ch] = planar_data[ch][i];
            }
        }

        // SDL 复制提交数据，下一轮可复用本数组；长度单位是字节而非帧。
        // 当前忽略提交返回值，失败不会回滚已经推进的图游标。
        SDL_PutAudioStreamData(audio_stream,
                               interleaved_buffer.data(),
                               interleaved_buffer.size() * sizeof(float));
    }
}

}  // namespace ice
