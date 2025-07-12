#include <ice/manage/AudioPool.hpp>

namespace ice {
AudioPool::AudioPool(CodecBackend codec_backend) {
    // 根据传入的backend初始化编解码器工厂
    switch (codec_backend) {
        case CodecBackend::FFMPEG: {
            // decoder_factory = std::make_unique<FFmpegDecoderFactory>();
            break;
        }
        case CodecBackend::COREAUDIO: {
            // decoder_factory = std::make_unique<CoreDecoderFactory>();
            break;
        }
    }
}
}  // namespace ice
