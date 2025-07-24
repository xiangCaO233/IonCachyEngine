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

namespace ice {
class AudioBuffer;

// 缓存策略
enum class CachingStrategy {
    // 完全缓存
    CACHY,
    // 流式
    STREAMING
};

class ThreadPool;
class AudioTrack {
   public:
    // 工厂方法
    [[nodiscard]] static std::shared_ptr<AudioTrack> create(
        std::string_view path, ThreadPool& thread_pool,
        std::shared_ptr<IDecoderFactory> decoder_factory,
        CachingStrategy strategy = ICEConfig::default_caching_strategy);

    // 禁止拷贝,唯一资源
    AudioTrack(const AudioTrack&) = delete;
    AudioTrack& operator=(const AudioTrack&) = delete;

    // 获取媒体信息
    inline const MediaInfo& get_media_info() const { return media_info; }

    // 获取文件绝对路径
    inline const std::string& path() const { return file_path; }

    // 将解码请求转发给其持有的解码器策略
    inline auto read(AudioBuffer& buffer, size_t start_frame,
                     size_t frame_count) const {
        return decoder->decode(buffer.raw_ptrs(), buffer.afmt.channels,
                               start_frame, frame_count);
    }

   private:
    // 私有构造函数，强制使用工厂方法
    AudioTrack(std::string_view p, ThreadPool& thread_pool,
               std::shared_ptr<IDecoderFactory> decoder_factory,
               CachingStrategy strategy);
    // 媒体信息
    MediaInfo media_info;

    // 音频路径
    std::string file_path;

    // 使用的解码器
    std::unique_ptr<IDecoder> decoder;
};

}  // namespace ice

#endif  // ICE_AUDIOTRACK_HPP
