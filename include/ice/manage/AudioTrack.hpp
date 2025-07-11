#ifndef ICE_AUDIOTRACK_HPP
#define ICE_AUDIOTRACK_HPP

#include <ice/manage/AudioFormat.hpp>
#include <memory>
#include <string_view>

#include "ice/manage/dec/IDecoder.hpp"

namespace ice {
class AudioBuffer;

enum class CachingStrategy {
    // 完全缓存
    CACHY,
    // 流式
    STREAMING
};

class AudioTrack {
   public:
    // 工厂方法，现在接受一个缓存策略
    [[nodiscard]] static std::shared_ptr<AudioTrack> create(
        std::string_view path, CachingStrategy strategy);

    // 禁止拷贝，这是一个唯一的资源
    AudioTrack(const AudioTrack&) = delete;
    AudioTrack& operator=(const AudioTrack&) = delete;

    // 原始数据格式
    inline const AudioDataFormat& native_format() const { return format; }

    // 原始音频路径
    inline const std::string& path() const { return file_path; }

    // 将解码请求转发给其持有的解码器策略
    inline size_t read(AudioBuffer& buffer, size_t start_frame,
                       size_t frame_count) const {
        return decoder->decode(buffer, start_frame, frame_count);
    }

   private:
    // 私有构造函数，强制使用工厂方法
    explicit AudioTrack(std::string p);

    // 音频路径
    std::string file_path;

    // 原始音频数据格式
    AudioDataFormat format;

    // 解码策略
    std::unique_ptr<IDecoder> decoder;
};

}  // namespace ice

#endif  // ICE_AUDIOTRACK_HPP
