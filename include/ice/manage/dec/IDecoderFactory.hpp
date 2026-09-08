#ifndef ICE_IDECODERFACTORY_HPP
#define ICE_IDECODERFACTORY_HPP

#include <ice/manage/AudioFormat.hpp>
#include <memory>
#include <string_view>

namespace ice
{
class IDecoderInstance;
class MediaInfo;

/// @brief 把媒体探测与有状态解码实例创建隔离，工厂本身不承载播放游标。
/// 具体工厂负责适配媒体后端，调用方不应直接访问后端上下文。
class IDecoderFactory
{
public:
    /// @brief 构造工厂接口部分，不打开媒体或创建解码状态。
    IDecoderFactory() = default;
    /// @brief 支持通过接口释放具体工厂，不代表其创建的实例也被销毁。
    virtual ~IDecoderFactory() = default;

    /// @brief 探测文件元信息，失败时调用方不能把输出当作完整有效媒体信息。
    /// @param file_path 调用期间借用的媒体路径，不转移字符串所有权。
    /// @param media_info 接收元信息的对象，失败不保证回滚已写字段。
    /// @return 探测是否成功，不代表之后重新打开同一路径必定成功。
    /// @warning 低频文件访问路径，不得在音频回调中进行探测。
    virtual bool probe(std::string_view file_path,
                       MediaInfo&       media_info) const = 0;

    /// @brief 为指定目标格式创建独立解码状态。
    /// @param file_path 路径视图只在调用期间有效，不移交字符串所有权。
    /// @param target_format 输出端要求的格式，后端负责必要的格式转换。
    /// @warning 可能打开文件、初始化编解码器并分配，必须放在非实时路径。
    /// @return 独占实例句柄；具体实现可返回空，历史实现也可能通过异常报告失败。
    /// 本接口未声明 noexcept，不能仅检查返回指针就声称覆盖所有失败通道。
    virtual std::unique_ptr<IDecoderInstance> create_instance(
        std::string_view            file_path,
        const ice::AudioDataFormat& target_format) const = 0;
};

}  // namespace ice

#endif  // ICE_IDECODERFACTORY_HPP
