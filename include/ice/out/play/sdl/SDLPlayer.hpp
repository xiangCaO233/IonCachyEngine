#ifndef ICE_SDLPLAYER_HPP
#define ICE_SDLPLAYER_HPP

#include <SDL3/SDL.h>

#include <atomic>
#include <thread>

#include "ice/out/IReceiver.hpp"
namespace ice {
struct AudioDeviceInfo {
    std::string name;      // 人类可读的设备名, e.g., "Realtek Digital Output"
    SDL_AudioDeviceID id;  // SDL/后端使用的设备ID
};
class SDLPlayer : public IReceiver {
   public:
    explicit SDLPlayer(const AudioDataFormat& format);
    // 状态管理
    // 打开sdl设备
    bool open() override;

    // 打开sdl设备
    bool open(SDL_AudioDeviceID deviceid);

    // 关闭sdl设备,释放资源
    void close() override;

    // 开始拉取数据并播放
    bool start() override;

    // 停止拉取数据
    void stop() override;

    // 查询状态
    bool is_running() const override;

   private:
    AudioDataFormat playformat;
    std::atomic<bool> paused{false};
    uint32_t chunk_size_frames;

    // 默认设备
    SDL_AudioDeviceID device_id = SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK;

    SDL_AudioStream* audio_stream = nullptr;

    // sdl推送循环线程
    std::thread sdl_audio_thread;
};
}  // namespace ice

#endif  // ICE_SDLPLAYER_HPP
