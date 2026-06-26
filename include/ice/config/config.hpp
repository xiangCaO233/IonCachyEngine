#ifndef ICE_CONFIG_HPP
#define ICE_CONFIG_HPP

#include <cstdint>

#include "ice/manage/AudioFormat.hpp"

#if defined(_WIN32) && defined(ICE_SHARED_LIBRARY)
#    if defined(ICE_BUILDING_LIBRARY)
#        define ICE_API __declspec(dllexport)
#    else
#        define ICE_API __declspec(dllimport)
#    endif
#else
#    define ICE_API
#endif

namespace ice
{
enum class CodecBackend;
enum class CachingStrategy;

/// @brief IonCachyEngine 全局音频配置。
class ICE_API ICEConfig
{
public:
    /// @brief 引擎内部混音和处理使用的音频格式。
    static AudioDataFormat internal_format;

    /// @brief 默认解码后端。
    static CodecBackend default_codec_backend;

    /// @brief 默认音频缓存策略。
    static CachingStrategy default_caching_strategy;

    /// @brief 默认音频缓冲帧数。
    static uint32_t default_buffer_size;
};

}  // namespace ice

#endif  // ICE_CONFIG_HPP
