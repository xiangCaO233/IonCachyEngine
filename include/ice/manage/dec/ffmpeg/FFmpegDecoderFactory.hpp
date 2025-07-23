#ifndef ICE_FFMPEGDECODERFACTORY_HPP
#define ICE_FFMPEGDECODERFACTORY_HPP

#include <ice/manage/dec/IDecoderFactory.hpp>
#include <memory>

namespace ice {
class FFmpegDecoderInstance;
class FFmpegDecoderFactory : public IDecoderFactory {
   public:
    // 探测文件元信息的通用接口
    void probe(std::string_view file_path,
               MediaInfo& media_info) const override;

    // 创建解码器实例接口
    std::unique_ptr<IDecoderInstance> create_instance(
        std::string_view file_path) const override;
};
}  // namespace ice

#endif  // ICE_FFMPEGDECODERFACTORY_HPP
