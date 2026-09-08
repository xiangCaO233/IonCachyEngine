#include "ice/manage/dec/StreamingDecoder.hpp"

namespace ice
{
// TODO(xiang 2025-07-12):实现流式解码器接口
/// @brief 只构造策略对象，非空结果不代表解码已就绪。
/// @warning 低频创建仍有对象分配和 shared_ptr 所有权复制，不能因没有 IO
/// 视作实时安全。
[[nodiscard]] std::unique_ptr<StreamingDecoder> StreamingDecoder::create(
    std::string_view path, const ice::AudioDataFormat& target_format,
    ThreadPool& thread_pool, std::shared_ptr<IDecoderFactory> factory)
{
    // 所有权交给调用方；这里尚未向线程池提交任务。
    return std::unique_ptr<StreamingDecoder>(
        new StreamingDecoder(path, target_format, thread_pool, factory));
}

/// @brief 参数用于未来的流式实现，当前构造不访问文件。
/// 不捕获路径、工厂或线程池，不对传入对象建立额外生命周期要求。
/// 按值工厂句柄仅在调用期间存在，未形成供后续读取使用的持久状态。
StreamingDecoder::StreamingDecoder(std::string_view            path,
                                   const ice::AudioDataFormat& target_format,
                                   ThreadPool&                 thread_pool,
                                   std::shared_ptr<IDecoderFactory> factory)
{
}

/// @brief 占位读取接口，保留调用方缓冲区原内容。
/// @warning 零返回值不能被解释为已经完成了一次有效解码。
/// @warning 每块读取的当前占位路径不等待、不分配，未来实现也不得阻塞补齐输入。
size_t StreamingDecoder::decode(float** buffer, uint16_t num_channels,
                                size_t start_frame, size_t frame_count)
{
    // TODO(xiang 2025-07-11): 实现流式解码
    // 不清零整块缓冲；有效帧数为零时由上层决定静音或结束策略。
    return 0;
}
/// @brief 流式缓存生命周期未定义前，不暴露借用的数据视图。
size_t StreamingDecoder::origin(
    std::vector<std::span<const float>>& origin_data, size_t start_frame,
    size_t frame_count)
{
    // 未持有稳定 PCM 缓存，因此不能向外发布有效的借用 span。
    // 输出容器保持原样，调用方不能把旧视图当成本次读取结果。
    return 0.;
}
}  // namespace ice
