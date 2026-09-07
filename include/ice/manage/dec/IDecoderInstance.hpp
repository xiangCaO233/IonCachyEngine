#ifndef ICE_IDECODERINSTANCE_HPP
#define ICE_IDECODERINSTANCE_HPP

#include <cstddef>
#include <ice/manage/AudioFormat.hpp>

namespace ice
{
/// @brief 有独立游标的顺序解码状态，不提供内部并发访问保证。
class IDecoderInstance
{
public:
    // 构造IDecoderInstance
    IDecoderInstance() = default;
    // 析构IDecoderInstance
    virtual ~IDecoderInstance() = default;
    /// @brief 定位帧位置；不同后端的精确定位能力由实现负责。
    /// @warning 定位与 read 必须由调用方串行化，失败需由上层决定恢复策略。
    virtual bool seek(size_t pos) = 0;
    /// @brief 将下一段样本写入调用方提供的浮点平面。
    /// @return 实际帧数，短读或零帧不保证一定是正常 EOF，需结合后端状态。
    virtual size_t read(float** buffer, size_t chunksize) = 0;

    // 抽象层解码所需信息接口
    virtual const AudioDataFormat& get_source_format() const       = 0;
    virtual size_t                 get_source_total_frames() const = 0;
};
}  // namespace ice

#endif  // ICE_IDECODERINSTANCE_HPP
