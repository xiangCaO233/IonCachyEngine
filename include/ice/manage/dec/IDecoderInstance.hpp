#ifndef ICE_IDECODERINSTANCE_HPP
#define ICE_IDECODERINSTANCE_HPP

#include <cstddef>
#include <ice/manage/AudioFormat.hpp>

namespace ice {
class IDecoderInstance {
   public:
    // 构造IDecoderInstance
    IDecoderInstance() = default;
    // 析构IDecoderInstance
    virtual ~IDecoderInstance() = default;
    // 定位帧位置的通用接口
    virtual bool seek(size_t pos) = 0;
    // 读取一块数据的通用接口
    virtual size_t read(float** buffer, size_t chunksize) = 0;

    // 抽象层解码所需信息接口
    virtual const AudioDataFormat& get_format() const = 0;
    virtual size_t get_total_frames() const = 0;
};
}  // namespace ice

#endif  // ICE_IDECODERINSTANCE_HPP
