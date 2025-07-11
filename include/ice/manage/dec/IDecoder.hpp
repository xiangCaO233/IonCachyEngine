#ifndef ICE_DECODER_HPP
#define ICE_DECODER_HPP

#include <cstddef>
#include <ice/manage/AudioBuffer.hpp>

namespace ice {
class AudioTrack;
class IDecoder {
    std::string_view file_path;

   public:
    // 构造IDecoder
    explicit IDecoder(std::string_view file) : file_path(file) {};

    // 析构IDecoder
    virtual ~IDecoder() = default;

    // 转移数据到缓冲区的接口
    virtual size_t decode(float** buffer, uint16_t num_channels,
                          size_t start_frame, size_t frame_count) = 0;
};

}  // namespace ice

#endif  // ICE_DECODER_HPP
