#ifndef ICE_ALPLAYER_HPP
#define ICE_ALPLAYER_HPP

#include <atomic>

#include "ice/out/IReceiver.hpp"

namespace ice {
class ALBackend;
class ALPlayer : public IReceiver {
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
    std::unique_ptr<ALBackend> sdlimpl;
    std::atomic<bool> paused{false};
};
}  // namespace ice

#endif  // ICE_ALPLAYER_HPP
