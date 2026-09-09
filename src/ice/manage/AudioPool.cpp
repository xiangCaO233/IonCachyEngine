#include <ice/manage/AudioPool.hpp>

#include "ice/manage/dec/ffmpeg/FFmpegDecoderFactory.hpp"

#include <memory>

namespace ice
{
/// @brief 按配置建立共享解码工厂，尚不打开媒体或启动解码。
/// @param codec_backend 当前实际实现仅支持 FFMPEG。
/// @warning 构造分配共享工厂，须位于非实时资源管理流程。
AudioPool::AudioPool(CodecBackend codec_backend)
{
    // 工厂由多个新音轨共享；具体媒体状态仍归各解码实例独占。
    switch ( codec_backend ) {
    case CodecBackend::FFMPEG: {
        // 工厂不保留当前媒体句柄，每个音轨建立各自的解码上下文。
        m_decoderFactory = std::make_shared<FFmpegDecoderFactory>();
        break;
    }
    case CodecBackend::COREAUDIO: {
        // 保留后端枚举但不创建实例，此分支留下空工厂，不能用于成功加载。
        // 调用方须选择已有实现；本构造没有返回错误的接口。
        // 不隐式回退到 FFmpeg，保持调用方的后端选择语义。
        break;
    }
    }
}
}  // namespace ice
