#ifndef ICE_DECODER_HPP
#define ICE_DECODER_HPP

#include <cstddef>

#include "ice/manage/AudioFormat.hpp"

namespace ice {
class AudioTrack;
class IDecoder {
   public:
    // 构造IDecoder
    IDecoder();
    // 析构IDecoder
    virtual ~IDecoder();

    // 获取音频文件原始格式
    virtual AudioFormat& format() = 0;

    // 获取用于读取数据的指针
    virtual const float* const* read(const AudioTrack& track,
                                     const AudioFormat& expected_format,
                                     size_t frames) = 0;
};

}  // namespace ice

#endif  // ICE_DECODER_HPP
