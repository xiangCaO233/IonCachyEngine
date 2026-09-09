#pragma once

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
    /// 成功时完整替换结果，缺失标签与封面清空；失败保留原输出。
    /// 封面只复制压缩字节，图像解码由消费元信息的一侧负责。
    bool probe(std::string_view file_path,
               MediaInfo&       media_info) const override;

    /// @brief 按目标输出格式创建解码实例，不复用 probe 的容器上下文。
    /// @warning 构造执行媒体打开及重采样准备，属于低频加载路径。
    /// FFmpeg 初始化失败返回空句柄，部分资源随候选实例销毁而释放。
    std::unique_ptr<IDecoderInstance> create_instance(
        std::string_view            file_path,
        const ice::AudioDataFormat& target_format) const override;
};
}  // namespace ice
