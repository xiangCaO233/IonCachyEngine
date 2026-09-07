#ifndef ICE_AUDIOTRACK_HPP
#define ICE_AUDIOTRACK_HPP

#include <cstddef>
#include <ice/config/config.hpp>
#include <ice/manage/AudioFormat.hpp>
#include <ice/manage/dec/IDecoder.hpp>
#include <ice/manage/dec/IDecoderFactory.hpp>
#include <ice/manage/dec/MediaInfo.hpp>
#include <memory>
#include <string>
#include <string_view>

namespace ice
{
class AudioBuffer;

// 缓存策略
enum class CachingStrategy {
    // 完全缓存
    CACHY,
    // 流式
    STREAMING
};

class ThreadPool;
/// @brief 共享媒体元信息与解码策略；播放游标由消费音轨的节点维护。
class AudioTrack
{
public:
    /// @brief 探测媒体并按缓存策略创建音轨，探测失败返回空句柄。
    /// @warning 文件探测及策略创建只允许在资源加载路径调用。
    [[nodiscard]] static std::shared_ptr<AudioTrack> create(
        std::string_view path, ThreadPool& thread_pool,
        std::shared_ptr<IDecoderFactory> decoder_factory,
        CachingStrategy strategy = ICEConfig::default_caching_strategy);

    // 禁止拷贝,唯一资源
    AudioTrack(const AudioTrack&)            = delete;
    AudioTrack& operator=(const AudioTrack&) = delete;

    /// @brief 返回探测阶段保存的元信息，而非实时读取进度。
    inline const MediaInfo& get_media_info() const { return media_info; }

    // 获取文件绝对路径
    inline const std::string& path() const { return file_path; }

    /// @brief 查询解码策略实际提供的帧数。
    /// @warning 完全缓存策略首次查询可能等待后台任务，须在播放前完成。
    inline size_t num_frames() const { return decoder->num_frames(); }

    /// @brief 将帧区间读取转发给音轨持有的解码策略。
    /// 不会调整缓冲格式或为调用方扩容。
    /// 调用方须保证缓冲容量足够，并按返回值处理未写入尾部。
    inline auto read(AudioBuffer& buffer, size_t start_frame,
                     size_t frame_count) const
    {
        // 此处不推进独立游标，因此多个节点可请求同一缓存的不同片段。
        return decoder->decode(
            buffer.raw_ptrs(), buffer.afmt.channels, start_frame, frame_count);
    }
    /// @brief 向容器追加只读 PCM 视图，音轨须活过这些视图。
    /// 当前位置参数最终转换为解码器使用的整数帧索引。
    /// @warning 输出容器可扩容；该接口不适合作为实时处理的首次准备入口。
    inline void origin(std::vector<std::span<const float>>& origin_data,
                       double start_frame, double frame_count)
    {
        // 保留解码器的追加语义；复用容器时由调用方清理旧的借用切片。
        decoder->origin(origin_data, start_frame, frame_count);
    }

private:
    // 私有构造函数，强制使用工厂方法
    AudioTrack(std::string_view p, ThreadPool& thread_pool,
               std::shared_ptr<IDecoderFactory> decoder_factory,
               CachingStrategy strategy, const MediaInfo& info);
    // 媒体信息
    MediaInfo media_info;

    // 音频路径
    std::string file_path;

    // 使用的解码器
    std::unique_ptr<IDecoder> decoder;
};

}  // namespace ice

#endif  // ICE_AUDIOTRACK_HPP
