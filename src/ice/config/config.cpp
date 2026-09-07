#include <ice/config/config.hpp>

#include "ice/manage/AudioPool.hpp"
#include "ice/manage/AudioTrack.hpp"

namespace ice
{
/// @brief 默认内部格式来自 AudioDataFormat 的成员默认值。
/// 修改全局默认值不会自动重配已创建的缓冲、解码器和播放器。
AudioDataFormat ICEConfig::internal_format{};
/// @brief 新建音轨默认使用 FFmpeg 后端，现有音轨保留已选后端。
CodecBackend ICEConfig::default_codec_backend{ CodecBackend::FFMPEG };
/// @brief 新建音轨默认缓存解码结果；流式路径由调用者显式选择。
CachingStrategy ICEConfig::default_caching_strategy{ CachingStrategy::CACHY };
/// @brief 默认块大小以帧计数，不包含声道倍数，也不是设备延迟保证。
uint32_t ICEConfig::default_buffer_size{ 1024 };

}  // namespace ice
