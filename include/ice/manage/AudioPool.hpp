#ifndef ICE_AUDIOPOOL_HPP
#define ICE_AUDIOPOOL_HPP

#include <string_view>

#include "ice/manage/AudioTrack.hpp"

namespace ice {
class AudioPool {
   public:
    // 构造AudioPool
    AudioPool();
    // 析构AudioPool
    virtual ~AudioPool() = default;

    // 载入文件到音频池
    const AudioTrack& load(std::string_view file);

   protected:
    AudioTrack unknown_track{"unknown"};
};
}  // namespace ice

#endif  // ICE_AUDIOPOOL_HPP
