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

    // 转移数据到缓冲区的接口(读取数量使用浮点数)
    virtual double decode(float** buffer, uint16_t num_channels,
                          double start_frame, double frame_count) = 0;
};

}  // namespace ice

#endif  // ICE_DECODER_HPP
