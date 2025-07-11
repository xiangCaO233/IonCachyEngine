#include <ice/manage/AudioTrack.hpp>

namespace ice {
// 工厂方法
[[nodiscard]] std::shared_ptr<AudioTrack> AudioTrack::create(
    std::string_view path, CachingStrategy strategy) {
    switch (strategy) {
        case CachingStrategy::CACHY: {
            break;
        }
        case CachingStrategy::STREAMING: {
            break;
        }
    }
    return std::make_shared<AudioTrack>(path);
};
// 私有构造函数，强制使用工厂方法
AudioTrack::AudioTrack(std::string path) : file_path(path) {}

}  // namespace ice
