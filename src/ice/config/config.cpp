#include <ice/config/config.hpp>

#include "ice/manage/AudioPool.hpp"
#include "ice/manage/AudioTrack.hpp"

namespace ice {
AudioDataFormat ICEConfig::internal_format;
CodecBackend ICEConfig::default_codec_backend{CodecBackend::FFMPEG};
CachingStrategy ICEConfig::default_caching_strategy{CachingStrategy::CACHY};

}  // namespace ice
