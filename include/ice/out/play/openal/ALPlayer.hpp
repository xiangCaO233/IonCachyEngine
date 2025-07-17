#ifndef ICE_ALPLAYER_HPP
#define ICE_ALPLAYER_HPP

#include <atomic>

#include "ice/out/IReceiver.hpp"

namespace ice {
struct ALCdevice;
struct ALAudioDeviceInfo {
    // 人类可读的设备名, e.g., "Realtek Digital Output"
    std::string name;
};
class ALBackend;
class ALPlayer : public IReceiver {
   public:
    static std::atomic<bool> al_inited;
    static bool init_backend();
    static void quit_backend();

    // 列出输出设备
    static std::vector<ALAudioDeviceInfo> list_devices();

    // 状态管理

    // 打开播放器(默认设备)
    bool open() override;
    // 打开播放器(指定设备)
    bool open(std::string_view devicename);

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
