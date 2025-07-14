#include <SDL3/SDL.h>
#include <SDL3/SDL_init.h>

#include <ice/out/play/sdl/SDLPlayer.hpp>

namespace ice {
class SDLBackend {
   public:
    bool init() {
        if (!SDL_Init(SDL_INIT_AUDIO)) {
            return false;
        }
        return true;
    };
};

bool SDLPlayer::open() { return sdlimpl->init(); }

// 关闭sdl设备,释放资源
void SDLPlayer::close() {}

// 开始拉取数据并播放
bool SDLPlayer::start() {}

// 停止拉取数据
void SDLPlayer::stop() { paused.store(true); }

// 查询状态
bool SDLPlayer::is_running() const { return !paused.load(); }

}  // namespace ice
