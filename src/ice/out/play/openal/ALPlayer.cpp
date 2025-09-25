#include <al.h>
#include <alc.h>
#include <fmt/base.h>

#include <ice/out/play/openal/ALPlayer.hpp>

namespace ice {
class ALBackend {};

std::atomic<bool> ALPlayer::al_inited{false};

bool ALPlayer::init_backend() { return true; }

void ALPlayer::quit_backend() {}

// 列出输出设备
std::vector<ALAudioDeviceInfo> ALPlayer::list_devices() {
    if (alcIsExtensionPresent(nullptr, "ALC_ENUMERATE_ALL_EXT") == AL_TRUE ||
        alcIsExtensionPresent(nullptr, "ALC_ENUMERATION_EXT") == AL_TRUE) {
        fmt::print("支持枚举al设备\n");
        const ALCchar* deviceList =
            alcGetString(nullptr, ALC_ALL_DEVICES_SPECIFIER);
        std::vector<ALAudioDeviceInfo> devices;
        while (*deviceList != '\0') {
            devices.emplace_back(std::string(deviceList));
            // 移动指针到下一个设备名称的开头
            // strlen(ptr) 会计算到第一个 '\0' 的长度
            deviceList += strlen(deviceList) + 1;
        }
        return devices;
    } else {
        // 不支持al枚举设备
        fmt::print("不支持枚举al设备\n");
        return {{alcGetString(nullptr, ALC_DEFAULT_DEVICE_SPECIFIER)}};
    }
}

// 状态管理
// 打开播放器(默认设备)
bool ALPlayer::open() {
    return open(alcGetString(nullptr, ALC_DEFAULT_DEVICE_SPECIFIER));
}
// 打开播放器(指定设备)
bool ALPlayer::open(std::string_view devicename) {
    auto device = alcOpenDevice(devicename.data());
    if (device) {
        auto ctx = alcCreateContext(device, nullptr);
        alcMakeContextCurrent(ctx);
        alGetError();
    }
    return true;
}

// 关闭sdl设备,释放资源
void ALPlayer::close() {}

// 开始拉取数据并播放
bool ALPlayer::start() { return true; }

// 停止拉取数据
void ALPlayer::stop() {}

// 查询状态
bool ALPlayer::is_running() const { return true; }

}  // namespace ice
