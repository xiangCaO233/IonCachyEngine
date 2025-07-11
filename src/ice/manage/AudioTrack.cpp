#include <ice/manage/AudioTrack.hpp>

#include "ice/manage/dec/CachyDecoder.hpp"
#include "ice/manage/dec/StreamingDecoder.hpp"

namespace ice {
// 工厂方法
[[nodiscard]] std::shared_ptr<AudioTrack> AudioTrack::create(
    std::string_view path, CachingStrategy strategy) {
    return std::shared_ptr<AudioTrack>(new AudioTrack(path, strategy));
};

// 私有构造函数，强制使用工厂方法
AudioTrack::AudioTrack(std::string_view path, CachingStrategy strategy)
    : file_path(path) {
    switch (strategy) {
        case CachingStrategy::CACHY: {
            decoder = std::make_unique<CachyDecoder>(path);
            break;
        }
        case CachingStrategy::STREAMING: {
            decoder = std::make_unique<StreamingDecoder>(path);
            break;
        }
    }
}

}  // namespace ice
