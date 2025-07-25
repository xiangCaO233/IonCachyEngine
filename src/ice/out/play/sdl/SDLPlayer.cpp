#include <SDL3/SDL_audio.h>
#include <SDL3/SDL_error.h>
#include <SDL3/SDL_init.h>
#include <fmt/base.h>

#include <ice/out/play/sdl/SDLPlayer.hpp>

#include "ice/config/config.hpp"
#include "ice/manage/AudioBuffer.hpp"

namespace ice {
std::atomic<bool> SDLPlayer::sdl_inited{false};
bool SDLPlayer::init_backend() {
    if (!sdl_inited.load()) {
        if (SDL_Init(SDL_INIT_AUDIO)) {
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

void SDLPlayer::quit_backend() {
    if (sdl_inited.load()) {
        SDL_Quit();
    }
}

// 列出输出设备
std::vector<SDLAudioDeviceInfo> SDLPlayer::list_devices() {
    std::vector<SDLAudioDeviceInfo> devices;
    int count = 0;
    if (auto sdl_devices = SDL_GetAudioPlaybackDevices(&count)) {
        for (int i = 0; i < count; ++i) {
            SDLAudioDeviceInfo info;
            info.id = sdl_devices[i];
            const char* name = SDL_GetAudioDeviceName(sdl_devices[i]);
            info.name = name ? name : "Unknown Device";
            devices.push_back(info);
        }
        SDL_free(sdl_devices);
    }

    return devices;
}

SDLPlayer::SDLPlayer(const AudioDataFormat& format)
    : IReceiver(format), playformat(format) {
    buffer.resize(playformat, ICEConfig::default_buffer_size);
}

bool SDLPlayer::open() { return open(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK); }

bool SDLPlayer::open(SDL_AudioDeviceID deviceid) {
    if (audio_stream) {
        // 如果已打开，先关闭
        close();
    }

    // 描述要提供的数据格式
    SDL_AudioSpec source_spec = {SDL_AUDIO_F32, playformat.channels,
                                 (int)playformat.samplerate};

    audio_stream =
        SDL_OpenAudioDeviceStream(deviceid, &source_spec, nullptr, nullptr);

    current_device = SDL_GetAudioStreamDevice(audio_stream);

    if (audio_stream == nullptr) {
        fmt::print("Failed to open audio device stream: {}\n", SDL_GetError());
        return false;
    }

    return true;
}

// 关闭sdl设备,释放资源
void SDLPlayer::close() {
    // 先停止线程和设备
    stop();

    // 对于由 SDL_OpenAudioDeviceStream 创建的流，
    // 调用 SDL_DestroyAudioStream 会自动关闭设备并处理解绑。
    // 你不需要做任何其他事情。
    if (audio_stream) {
        SDL_DestroyAudioStream(audio_stream);
        audio_stream = nullptr;
    }
    // 确保设备 ID 被重置
    current_device = 0;
}

// 开始拉取数据并播放
bool SDLPlayer::start() {
    if (running.load() || !audio_stream) {
        return false;
    }
    running.store(true);
    paused.store(false);

    // 恢复设备，以防之前被暂停过
    SDL_ResumeAudioDevice(SDL_GetAudioStreamDevice(audio_stream));

    // 在恢复设备后再创建线程
    sdl_audio_thread = std::thread(&SDLPlayer::audio_thread_loop, this);

    return true;
}

bool SDLPlayer::joinable() { return sdl_audio_thread.joinable(); }

void SDLPlayer::join() { sdl_audio_thread.join(); }

// 停止拉取数据
void SDLPlayer::stop() {
    if (!running.load()) {
        return;
    }

    // 1. 设置标志，通知线程退出
    running.store(false);

    // 2. 如果线程因为我们的 pause() 调用而卡住，唤醒它
    //    这里我们直接 resume 设备，让 SDL 的回调机制能继续，
    //    从而让我们的线程能从可能的阻塞中返回。
    //    这一步是解决“暂停后退出崩溃”的关键！
    if (paused.load()) {
        resume();  // 调用我们自己的 resume 函数
    }

    // 3. 等待音频线程自然结束
    if (sdl_audio_thread.joinable()) {
        sdl_audio_thread.join();
    }
}
void SDLPlayer::pause() {
    if (running.load() && !paused.load()) {
        paused.store(true);
        // 直接使用 SDL 的函数来暂停物理设备
        SDL_PauseAudioDevice(SDL_GetAudioStreamDevice(audio_stream));
    }
}
void SDLPlayer::resume() {
    if (running.load() && paused.load()) {
        paused.store(false);
        // 恢复物理设备
        SDL_ResumeAudioDevice(SDL_GetAudioStreamDevice(audio_stream));
    }
}

// sdl音频线程函数
void SDLPlayer::audio_thread_loop() {
    // 我们的目标是让SDL的内部缓冲区，始终保持大约2-4个数据块的量。
    // 这在延迟和稳定性之间提供了完美的平衡。
    // 比如维持3个块的缓冲
    const int target_queued_bytes =
        buffer.num_frames() * playformat.channels * sizeof(float) * 3;
    // 创建一个临时的、用于存放交错数据的缓冲区
    std::vector<float> interleaved_buffer(buffer.num_frames() *
                                          playformat.channels);
    while (running.load()) {
        // --- 检查库存 ---
        int queued_bytes = SDL_GetAudioStreamQueued(audio_stream);

        // 如果库存充足，就没必要现在补货。休眠一小会儿，避免CPU空转。
        if (queued_bytes > target_queued_bytes) {
            if (!running.load()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            // 继续下一次循环检查
            continue;
        }

        // --- 库存不足，开始补货 ---
        // --- 拉取数据 ---
        {
            std::scoped_lock<std::mutex> lock(source_mutex);
            buffer.clear();
            if (get_source()) {
                // 从连接的音频图中拉取标准格式的数据
                get_source()->process(buffer);
            }
            // 如果没有数据源,直接输出静音
        }
        // 锁在这里被释放
        // --- 数据格式转换 (平面 -> 交错) ---
        const auto num_frames = buffer.num_frames();
        const auto num_channels = playformat.channels;
        auto planar_data = buffer.raw_ptrs();

        for (size_t i = 0; i < num_frames; ++i) {
            for (uint16_t ch = 0; ch < num_channels; ++ch) {
                interleaved_buffer[i * num_channels + ch] = planar_data[ch][i];
            }
        }

        // --- 推送数据 ---
        SDL_PutAudioStreamData(audio_stream, interleaved_buffer.data(),
                               interleaved_buffer.size() * sizeof(float));
    }
}

}  // namespace ice
