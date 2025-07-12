#ifndef ICE_FFMPEGDECODERINSTANCE_HPP
#define ICE_FFMPEGDECODERINSTANCE_HPP

#include "ice/manage/dec/IDecoderInstance.hpp"

namespace ice {
class FFmpegDecoderInstance : public IDecoderInstance {
   public:
    // 定位帧位置
    bool seek(size_t pos) override;
    // 读取一块数据
    size_t read(float** buffer, size_t chunksize) override;
};
}  // namespace ice

#endif  // ICE_FFMPEGDECODERINSTANCE_HPP
