#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace ice
{
class AudioTrack;
/// @brief 提供区间复制或借用视图的上层解码访问接口。
/// 缓存实现与流式实现可以不同，不承诺所有实现均支持零拷贝 origin。
/// 接口没有准备状态、错误原因或取消通道；零返回值不能统一解释为正常文件结束。
/// 线程安全及首次调用是否等待由具体实现说明，虚函数接口不提供同步保护。
class IDecoder
{
public:
    /// @brief 构造无状态访问接口，不探测媒体或准备解码资源。
    explicit IDecoder() = default;

    /// @brief 允许通过接口释放实现对象，不保证外部借用视图在销毁后仍有效。
    virtual ~IDecoder() = default;

    /// @brief 查询实现当前报告的每声道总帧数，不是总样本数或毫秒时长。
    /// @return 具体实现定义的帧数；零也可能表示不支持或加载失败。
    /// @warning 缓存实现首次查询可能等待，不能仅因 const
    /// 就在实时路径未经准备地调用。
    virtual size_t num_frames() const = 0;

    /// @brief 将区间内的浮点样本复制到调用方提供的声道平面。
    /// @param buffer 可写声道指针表，调用方拥有目标存储并保证容量。
    /// @param num_channels 目标声道数，补齐或裁剪规则由具体实现规定。
    /// @param start_frame 起始帧索引，不是字节偏移。
    /// @param frame_count 请求帧数；实际可读量可能少于请求。
    /// @return 本次区间可用帧数，调用方负责处理未写满的后缀及未覆盖声道。
    /// 零目标声道等边界情况依实现而定，不可把返回值换算成实际写入总样本数。
    /// @warning
    /// 音频读取调用链必须先满足实现的准备约束，不允许增加临时分配或阻塞。
    virtual size_t decode(float** buffer, uint16_t num_channels,
                          size_t start_frame, size_t frame_count) = 0;

    /// @brief 获取内部音频区间的只读借用视图，避免重复复制。
    /// @param origin_data 接收声道视图的容器；追加还是替换由具体实现说明。
    /// @param start_frame 区间起始帧索引。
    /// @param frame_count 每声道请求帧数。
    /// @warning span 不拥有样本；使用期间必须保留解码器及其底层存储。
    /// 零拷贝不承诺无分配或无等待，容器扩容及缓存初始化仍可能发生。
    /// @return 成功交付的区间帧数；不支持此操作的实现可返回零。
    virtual size_t origin(std::vector<std::span<const float>>& origin_data,
                          size_t start_frame, size_t frame_count) = 0;
};

}  // namespace ice
