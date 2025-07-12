#ifndef ICE_IDECODERINSTANCE_HPP
#define ICE_IDECODERINSTANCE_HPP

#include <cstddef>
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
};
}  // namespace ice

#endif  // ICE_IDECODERINSTANCE_HPP
