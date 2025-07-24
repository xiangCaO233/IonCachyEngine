#ifndef ICE_SDLPLAYER_HPP
#define ICE_SDLPLAYER_HPP

#include <SDL3/SDL.h>

#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

#include "ice/config/config.hpp"
#include "ice/out/IReceiver.hpp"

namespace ice {
struct SDLAudioDeviceInfo {
    // 人类可读的设备名, e.g., "Realtek Digital Output"
    std::string name;
    // SDL/后端使用的设备ID
    SDL_AudioDeviceID id;
};
class SDLPlayer : public IReceiver {
   public:
    explicit SDLPlayer(
        const AudioDataFormat& format = ICEConfig::internal_format);
    static std::atomic<bool> sdl_inited;
    static bool init_backend();
    static void quit_backend();

    // 列出输出设备
    static std::vector<SDLAudioDeviceInfo> list_devices();

    // 状态管理
    // 打开sdl设备
    bool open() override;

    // 打开sdl设备
    bool open(SDL_AudioDeviceID deviceid);

    // 关闭sdl设备,释放资源
    void close() override;

    // 开始拉取数据并播放
    bool start() override;

    bool joinable();

    void join();

    // 停止拉取数据
    void stop() override;

    // 查询状态
    bool is_running() const override { return running.load(); }

    // sdl音频线程函数
    void audio_thread_loop();

    inline SDL_AudioDeviceID get_current_device() const {
        return current_device;
    }

   private:
    // 播放器格式
    AudioDataFormat playformat;

    // 播放器内部缓冲区
    AudioBuffer buffer;

    // 播放器是否正在播放
    std::atomic<bool> running{false};

    // 当前播放设备
    SDL_AudioDeviceID current_device{0};

    // sdl音频流
    SDL_AudioStream* audio_stream{nullptr};

    // sdl推送循环线程
    std::mutex source_mutex;
    std::thread sdl_audio_thread;
};
}  // namespace ice

#endif  // ICE_SDLPLAYER_HPP
