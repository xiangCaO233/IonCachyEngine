#ifndef ICE_FFMPEGDECODERINSTANCE_HPP
#define ICE_FFMPEGDECODERINSTANCE_HPP

#include <ice/manage/dec/IDecoderInstance.hpp>
#include <memory>

#include "ice/manage/AudioFormat.hpp"

namespace ice
{
class FFmpegDecoder;
/// @brief 独占 FFmpeg 解码上下文，提供平面浮点 PCM 读取。
/// 同一实例的 seek 与 read 必须串行调用，不能跨线程并发操作。
class FFmpegDecoderInstance : public IDecoderInstance
{
public:
    /// @brief 打开媒体并准备目标格式转换。
    /// @warning 初始化涉及文件访问和内存分配，不允许在音频热路径调用。
    /// 失败仍可能传播既有异常；后端构造失败的裸句柄回滚尚非完整 RAII。
    explicit FFmpegDecoderInstance(std::string_view            file_path,
                                   const ice::AudioDataFormat& target_format);

    /// @brief 在实现类型完整可见的翻译单元中释放后端上下文。
    ~FFmpegDecoderInstance() override;

    /// @brief 定位到目标输出采样率对应的帧位置，并清除旧解码状态。
    /// 向后寻道不裁剪关键帧到目标位置之间的前导采样，不保证逐帧精确。
    /// @warning 仅在非实时解码侧串行调用，可能进行文件 IO 与重采样状态重建。
    bool seek(size_t pos) override;
    /// @brief 读取至多 chunksize 帧，buffer 为调用方预分配的声道指针表。
    /// 返回短读可能代表 EOF 或错误，未写入的尾部由调用方处理。
    /// @warning 解码可能扩容和读取文件，不可在实时设备回调直接调用。
    /// 当前 EOF 排尾未保留超出本次请求的重采样帧，小块末尾读取存在丢尾风险。
    size_t read(float** buffer, size_t chunksize) override;

    /// @brief 返回实例报告的 PCM 输出格式，不是压缩包的编码格式。
    const AudioDataFormat& get_source_format() const override;
    /// @brief 返回初始化阶段估算的帧数，实际读到的长度可与之不同。
    size_t get_source_total_frames() const override;

private:
    /// @brief 唯一拥有后端，公开头不暴露 FFmpeg 类型或资源释放细节。
    std::unique_ptr<FFmpegDecoder> ffimpl;
};
}  // namespace ice

#endif  // ICE_FFMPEGDECODERINSTANCE_HPP
