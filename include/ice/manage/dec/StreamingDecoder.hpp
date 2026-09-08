#ifndef ICE_STREAMINGDECODER_HPP
#define ICE_STREAMINGDECODER_HPP

#include <ice/manage/dec/IDecoderFactory.hpp>
#include <ice/thread/ThreadPool.hpp>
#include <memory>

#include "ice/manage/dec/IDecoder.hpp"

namespace ice
{
/// @brief 流式解码策略的占位实现，目前不提供音频帧。
/// 非空对象不代表媒体可播放，调用方仍须检查实际读取帧数。
class StreamingDecoder : public IDecoder
{
public:
    /// @brief 创建占位对象；当前不会启动解码任务或探测媒体。
    /// @param path 当前不使用、不保存的媒体路径。
    /// @param target_format 当前不验证、不保存的格式请求。
    /// @param thread_pool 当前不提交任务的外部线程池。
    /// @param factory 按值传入但不保存的工厂句柄。
    /// @return 仅具有接口形状的占位对象，不可作为流式能力就绪标志。
    /// @warning 创建仍分配对象并复制共享句柄，不应在音频回调调用。
    [[nodiscard]] static std::unique_ptr<StreamingDecoder> create(
        std::string_view path, const ice::AudioDataFormat& target_format,
        ThreadPool& thread_pool, std::shared_ptr<IDecoderFactory> factory);

    /// @brief 当前始终返回零，不改写调用方缓冲区。
    /// 起始位置与请求长度的单位是每声道采样帧。
    /// 后续实现须区分暂时缺数据与文件结束，不能靠阻塞等待补齐。
    /// @warning 当前热路径仅直接返回，不触发 IO
    /// 或等待；不得以同步预读补齐占位实现。
    size_t decode(float** buffer, uint16_t num_channels, size_t start_frame,
                  size_t frame_count) override;

    /// @brief 当前不支持直接借用 PCM，返回零且不追加视图。
    /// 未定义稳定缓存生命周期前，不能返回临时解码块的地址。
    /// @return 固定为零；容器已有元素不属于本次请求结果。
    size_t origin(std::vector<std::span<const float>>& origin_data,
                  size_t start_frame, size_t frame_count) override;

    /// @brief 占位阶段没有已解码帧，返回零。
    /// 此值并非文件元信息中的时长估算。
    size_t num_frames() const override { return 0; }

private:
    /// @brief 保留流式策略所需参数签名，目前不保存参数或启动工作线程。
    /// @warning 尚无预读、定位和取消协议，不能用于实时流式播放。
    explicit StreamingDecoder(std::string_view                 path,
                              const ice::AudioDataFormat&      target_format,
                              ThreadPool&                      thread_pool,
                              std::shared_ptr<IDecoderFactory> factory);
};

}  // namespace ice

#endif  // ICE_STREAMINGDECODER_HPP
