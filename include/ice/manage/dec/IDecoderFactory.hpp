#ifndef ICE_IDECODERFACTORY_HPP
#define ICE_IDECODERFACTORY_HPP

#include <memory>
#include <string_view>

namespace ice {
class IDecoderInstance;
class MediaInfo;

class IDecoderFactory {
   public:
    // 构造IDecoderFactory
    IDecoderFactory() = default;
    // 析构IDecoderFactory
    virtual ~IDecoderFactory() = default;

    // 探测文件元信息的通用接口
    virtual void probe(std::string_view file_path,
                       MediaInfo& media_info) const = 0;

    // 创建解码器实例接口
    virtual std::unique_ptr<IDecoderInstance> create_instance(
        std::string_view file_path) const = 0;
};

}  // namespace ice

#endif  // ICE_IDECODERFACTORY_HPP
