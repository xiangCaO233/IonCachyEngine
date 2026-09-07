#ifndef ICE_CONFIG_HPP
#define ICE_CONFIG_HPP

#include <cstdint>

#include "ice/manage/AudioFormat.hpp"

// Windows 动态库区分导出与导入；静态链接和非 Windows 平台不添加此修饰。
// ICE_BUILDING_LIBRARY 只由引擎目标自身定义，不能传播给库的使用者。
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
/// @warning 这些静态字段不是原子状态，必须在处理线程启动前完成配置。
/// 配置变化不会自动重建已经创建的解码缓存、设备或效果器。
class ICE_API ICEConfig
{
public:
    /// @brief 引擎内部混音和处理使用的音频格式。
    static AudioDataFormat internal_format;

    /// @brief 默认解码后端。
    /// 仅作为创建资源时的选择，不切换现有解码实例。
    static CodecBackend default_codec_backend;

    /// @brief 默认音频缓存策略。
    /// 完全缓存与流式策略的可用性由对应解码实现决定。
    static CachingStrategy default_caching_strategy;

    /// @brief 默认音频缓冲帧数。
    /// 单位为每声道采样帧，不是全部声道累加的样本数或字节数。
    static uint32_t default_buffer_size;
};

}  // namespace ice

#endif  // ICE_CONFIG_HPP
