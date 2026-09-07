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
    [[nodiscard]] static std::unique_ptr<StreamingDecoder> create(
        std::string_view path, const ice::AudioDataFormat& target_format,
        ThreadPool& thread_pool, std::shared_ptr<IDecoderFactory> factory);

    /// @brief 当前始终返回零，不改写调用方缓冲区。
    /// 起始位置与请求长度的单位是每声道采样帧。
    /// 后续实现须区分暂时缺数据与文件结束，不能靠阻塞等待补齐。
    size_t decode(float** buffer, uint16_t num_channels, size_t start_frame,
                  size_t frame_count) override;

    /// @brief 当前不支持直接借用 PCM，返回零且不追加视图。
    /// 未定义稳定缓存生命周期前，不能返回临时解码块的地址。
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
