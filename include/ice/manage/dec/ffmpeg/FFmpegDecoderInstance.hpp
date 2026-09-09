#pragma once

#include <cstddef>
#include <ice/manage/dec/IDecoderInstance.hpp>
#include <memory>
#include <string_view>

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
    /// C 接口失败形成无效实例；read 返回零，seek 返回 false，析构释放部分资源。
    explicit FFmpegDecoderInstance(std::string_view            file_path,
                                   const ice::AudioDataFormat& target_format);

    /// @brief 在实现类型完整可见的翻译单元中释放后端上下文。
    ~FFmpegDecoderInstance() override;

    /// @brief 返回解码资源是否可用；初始化或寻道重启失败后为 false。
    bool isValid() const;

    /// @brief 查询最近一次初始化或定位诊断点的 FFmpeg 负错误码。
    /// 零表示没有已记录诊断，不保证实例有效；参数、分配及 read 错误未全面覆盖。
    /// 成功操作不清除此值，须结合当前操作返回值判断是否发生新失败。
    /// 与 seek/read 串行调用；只返回整数，由调用方决定如何格式化及输出。
    int getLastSetupErrorCode() const;

    /// @brief 定位到目标输出采样率对应的帧位置，并清除旧解码状态。
    /// 向后寻道不裁剪关键帧到目标位置之间的前导采样，不保证逐帧精确。
    /// 零帧定位先准备新的编解码上下文；成功定位后替换旧状态以重建编码历史。
    /// 重采样重启失败后实例失效，后续读取返回零，须重新创建实例。
    /// @warning 仅在非实时解码侧串行调用，可能进行文件 IO 与重采样状态重建。
    bool seek(size_t pos) override;
    /// @brief 读取至多 chunksize 帧，buffer 为调用方预分配的声道指针表。
    /// 返回短读可能代表 EOF 或错误，未写入的尾部由调用方处理。
    /// @warning 解码可能扩容和读取文件，不可在实时设备回调直接调用。
    /// EOF 排尾复用普通 PCM 余量缓存，超出本次请求的帧留给后续 read。
    /// 短读仍不区分正常排尾结束与后端错误，不能作为完整文件已解码的证明。
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
