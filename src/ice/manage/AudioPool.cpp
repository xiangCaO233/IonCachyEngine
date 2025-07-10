#include <ice/manage/AudioPool.hpp>

namespace ice {
AudioPool::AudioPool() {}

const AudioTrack& AudioPool::load(std::string_view file) {
    return unknown_track;
}

}  // namespace ice
