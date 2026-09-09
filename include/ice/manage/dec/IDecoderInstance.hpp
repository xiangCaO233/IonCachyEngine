#pragma once

#include <cstddef>

namespace ice
{
/// @brief 仅借用音频格式，接口声明不需要其字段布局。
struct AudioDataFormat;

/// @brief 有独立游标的顺序解码状态，不提供内部并发访问保证。
class IDecoderInstance
{
public:
    /// @brief 构造解码实例接口，不在基类中打开文件。
    IDecoderInstance() = default;
    /// @brief 释放派生实例，调用方须先停止对此实例的定位和读取。
    virtual ~IDecoderInstance() = default;
    /// @brief 定位帧位置；不同后端的精确定位能力由实现负责。
    /// @param pos 由具体实现解释的帧位置，不能不经核对就混用源与目标采样率。
    /// @return 定位请求是否被接受，不保证下一次读取从精确样本边界开始。
    /// @warning 定位与 read 必须由调用方串行化，失败需由上层决定恢复策略。
    virtual bool seek(size_t pos) = 0;
    /// @brief 将下一段样本写入调用方提供的浮点平面。
    /// @param buffer 按实例输出格式提供的可写声道指针表，各声道容量至少为
    /// chunksize。
    /// @param chunksize 每声道请求帧数，不是字节数或全部声道样本总和。
    /// @warning
    /// 可能执行解码、文件读取或内部缓冲操作，不承诺适合直接在音频回调调用。
    /// @return 实际帧数，短读或零帧不保证一定是正常 EOF，需结合后端状态。
    virtual size_t read(float** buffer, size_t chunksize) = 0;

    /// @brief 借用实例报告的音频格式，具体后端可能报告转换后的输出格式。
    /// @return 实例拥有的格式引用，不可跨越实例销毁。
    /// 名称中的 source 不保证与原始容器格式一致，调用方须核对后端约定。
    virtual const AudioDataFormat& get_source_format() const = 0;
    /// @brief 查询实例报告的总帧数估计，不等同于累计 read 返回值。
    /// @return 后端定义的长度；采样率单位及未知长度表示应按具体实现解释。
    virtual size_t get_source_total_frames() const = 0;
};
}  // namespace ice
