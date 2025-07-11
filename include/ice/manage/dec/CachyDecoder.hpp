#ifndef ICE_CACHYDECODER_HPP
#define ICE_CACHYDECODER_HPP

#include "ice/manage/dec/IDecoder.hpp"

namespace ice {
class CachyDecoder : public IDecoder {
   public:
    // 构造CachyDecoder
    CachyDecoder();

    // 解码数据到缓冲区的接口
    size_t decode(float** buffer, size_t start_frame,
                  size_t frame_count) override;
};

}  // namespace ice

#endif  // ICE_CACHYDECODER_HPP
