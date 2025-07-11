#ifndef ICE_DECODER_HPP
#define ICE_DECODER_HPP

#include <cstddef>
#include <ice/manage/AudioBuffer.hpp>

namespace ice {
class AudioTrack;
class IDecoder {
   public:
    // 构造IDecoder
    IDecoder() = default;
    // 析构IDecoder
    virtual ~IDecoder() = default;

    // 解码数据到缓冲区的接口
    virtual size_t decode(float** buffer, size_t start_frame,
                          size_t frame_count) = 0;
};

}  // namespace ice

#endif  // ICE_DECODER_HPP
