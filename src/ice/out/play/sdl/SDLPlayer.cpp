
#include <ice/out/play/sdl/SDLPlayer.hpp>

#include "ice/execptions/load_error.hpp"

namespace ice {

SDLPlayer::SDLPlayer(const AudioDataFormat &format)
    : IReceiver(format), playformat(format) {
    // 创建一个音频流，告诉SDL提供什么格式的数据-和目标播放的格式
    SDL_AudioSpec src_spec{SDL_AUDIO_F32, format.channels,
                           (int)format.samplerate};
    SDL_AudioSpec dst_spec = src_spec;
    audio_stream = SDL_CreateAudioStream(&src_spec, &dst_spec);
    if (audio_stream == nullptr) {
        throw load_error("create sdl stream failed");
    }
}

bool SDLPlayer::open() { return open(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK); }
bool SDLPlayer::open(SDL_AudioDeviceID deviceid) {
    // 如果已经打开了，先关闭
    if (device_id != 0) {
        close();
    }

    SDL_AudioSpec spec{SDL_AUDIO_F32, playformat.channels,
                       (int)playformat.samplerate};

    // 打开指定的播放设备
    device_id = SDL_OpenAudioDevice(deviceid, &spec);

    if (device_id == 0) {
        return false;
    }

    return true;
}

// 关闭sdl设备,释放资源
void SDLPlayer::close() {}

// 开始拉取数据并播放
bool SDLPlayer::start() {}

// 停止拉取数据
void SDLPlayer::stop() { paused.store(true); }

// 查询状态
bool SDLPlayer::is_running() const { return !paused.load(); }

}  // namespace ice
