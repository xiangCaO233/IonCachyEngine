#ifndef ICE_SDLPLAYER_HPP
#define ICE_SDLPLAYER_HPP

#include <atomic>
#include <memory>

#include "ice/out/IReceiver.hpp"
namespace ice {
class SDLBackend;
class SDLPlayer : public IReceiver {
   public:
    // 状态管理
    // 打开sdl设备
    bool open() override;

    // 关闭sdl设备,释放资源
    void close() override;

    // 开始拉取数据并播放
    bool start() override;

    // 停止拉取数据
    void stop() override;

    // 查询状态
    bool is_running() const override;

   private:
    std::unique_ptr<SDLBackend> sdlimpl;
    std::atomic<bool> paused{false};
};
}  // namespace ice

#endif  // ICE_SDLPLAYER_HPP
