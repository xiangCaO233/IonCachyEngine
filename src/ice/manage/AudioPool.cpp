#include <ice/manage/AudioPool.hpp>

#include "ice/manage/dec/ffmpeg/FFmpegDecoderFactory.hpp"

namespace ice
{
AudioPool::AudioPool(CodecBackend codec_backend)
{
    // 根据传入的backend初始化编解码器工厂
    switch ( codec_backend )
        {
        case CodecBackend::FFMPEG:
            {
                decoder_factory = std::make_shared<FFmpegDecoderFactory>();
                break;
            }
        case CodecBackend::COREAUDIO:
            {
                // decoder_factory = std::make_shared<CoreDecoderFactory>();
                break;
            }
        }
}
}  // namespace ice
