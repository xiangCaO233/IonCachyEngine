#ifndef ICE_CONFIG_HPP
#define ICE_CONFIG_HPP

#include "ice/manage/AudioFormat.hpp"

namespace ice {
enum class CodecBackend;
enum class CachingStrategy;

class ICEConfig {
   public:
    static AudioDataFormat internal_format;
    static CodecBackend default_codec_backend;
    static CachingStrategy default_caching_strategy;
};

}  // namespace ice

#endif  // ICE_CONFIG_HPP
