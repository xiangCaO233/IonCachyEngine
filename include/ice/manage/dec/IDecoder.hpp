#ifndef ICE_DECODER_HPP
#define ICE_DECODER_HPP

#include <cstddef>
#include <ice/manage/AudioBuffer.hpp>

namespace ice {
class AudioTrack;
class IDecoder {
   public:
    // 构造IDecoder
    explicit IDecoder() = default;

    // 析构IDecoder
    virtual ~IDecoder() = default;

    // 获取总帧数量接口
    virtual size_t num_frames() const = 0;

    // 转移数据到缓冲区的接口
    virtual size_t decode(float** buffer, uint16_t num_channels,
                          size_t start_frame, size_t frame_count) = 0;
};

}  // namespace ice

#endif  // ICE_DECODER_HPP
