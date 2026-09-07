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
    // 构造IDecoderFactory
    IDecoderFactory() = default;
    // 析构IDecoderFactory
    virtual ~IDecoderFactory() = default;

    /// @brief 探测文件元信息，失败时调用方不能把输出当作完整有效媒体信息。
    /// @warning 低频文件访问路径，不得在音频回调中进行探测。
    virtual bool probe(std::string_view file_path,
                       MediaInfo&       media_info) const = 0;

    /// @brief 为指定目标格式创建独立解码状态，失败以空所有者表达。
    /// @param file_path 路径视图只在调用期间有效，不移交字符串所有权。
    /// @param target_format 输出端要求的格式，后端负责必要的格式转换。
    /// @warning 可能打开文件、初始化编解码器并分配，必须放在非实时路径。
    virtual std::unique_ptr<IDecoderInstance> create_instance(
        std::string_view            file_path,
        const ice::AudioDataFormat& target_format) const = 0;
};

}  // namespace ice

#endif  // ICE_IDECODERFACTORY_HPP
