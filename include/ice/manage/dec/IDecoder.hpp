#ifndef ICE_DECODER_HPP
#define ICE_DECODER_HPP

#include <cstddef>
#include <ice/manage/AudioBuffer.hpp>
#include <span>

namespace ice
{
class AudioTrack;
/// @brief 提供区间复制或借用视图的上层解码访问接口。
/// 缓存实现与流式实现可以不同，不承诺所有实现均支持零拷贝 origin。
class IDecoder
{
public:
    // 构造IDecoder
    explicit IDecoder() = default;

    // 析构IDecoder
    virtual ~IDecoder() = default;

    // 获取总帧数量接口
    virtual size_t num_frames() const = 0;

    /// @brief 将区间内的浮点样本复制到调用方提供的声道平面。
    /// @param start_frame 起始帧索引，不是字节偏移。
    /// @param frame_count 请求帧数；实际可读量可能少于请求。
    /// @return 实际交付帧数，调用方负责处理未写满的后缀。
    virtual size_t decode(float** buffer, uint16_t num_channels,
                          size_t start_frame, size_t frame_count) = 0;

    /// @brief 获取内部音频区间的只读借用视图，避免重复复制。
    /// @warning span 不拥有样本；使用期间必须保留解码器及其底层存储。
    /// @return 成功交付的区间帧数；不支持此操作的实现可返回零。
    virtual size_t origin(std::vector<std::span<const float>>& origin_data,
                          size_t start_frame, size_t frame_count) = 0;
};

}  // namespace ice

#endif  // ICE_DECODER_HPP
