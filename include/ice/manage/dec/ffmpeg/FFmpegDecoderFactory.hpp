#ifndef ICE_FFMPEGDECODERFACTORY_HPP
#define ICE_FFMPEGDECODERFACTORY_HPP

#include <ice/manage/dec/IDecoderFactory.hpp>
#include <memory>

namespace ice
{
class FFmpegDecoderInstance;
/// @brief 以 FFmpeg 探测容器并创建独立的音频解码实例。
class FFmpegDecoderFactory : public IDecoderFactory
{
public:
    /// @brief 读取首个音频流及容器元信息，失败返回 false。
    /// @warning 文件探测会阻塞并可能分配封面字节，不能在音频回调中调用。
    /// 未出现的标签不覆盖原字段；调用方应传入新的元信息对象。
    /// 封面只复制压缩字节，图像解码由消费元信息的一侧负责。
    bool probe(std::string_view file_path,
               MediaInfo&       media_info) const override;

    /// @brief 按目标输出格式创建解码实例，不复用 probe 的容器上下文。
    /// @warning 构造执行媒体打开及重采样准备，属于低频加载路径。
    /// 当前初始化失败沿用历史异常通道，并非总是返回空句柄。
    std::unique_ptr<IDecoderInstance> create_instance(
        std::string_view            file_path,
        const ice::AudioDataFormat& target_format) const override;
};
}  // namespace ice

#endif  // ICE_FFMPEGDECODERFACTORY_HPP
