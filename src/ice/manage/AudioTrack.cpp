#include <ice/manage/AudioTrack.hpp>
#include <memory>

#include "ice/core/SourceNode.hpp"
#include "ice/manage/dec/CachyDecoder.hpp"
#include "ice/manage/dec/StreamingDecoder.hpp"

namespace ice {
// 工厂方法
[[nodiscard]] std::shared_ptr<AudioTrack> AudioTrack::create(
    std::string_view path, ThreadPool& thread_pool,
    std::shared_ptr<IDecoderFactory> decoder_factory,
    CachingStrategy strategy) {
    return std::shared_ptr<AudioTrack>(
        new AudioTrack(path, thread_pool, decoder_factory, strategy));
};

// 私有构造函数，强制使用工厂方法
AudioTrack::AudioTrack(std::string_view path, ThreadPool& thread_pool,
                       std::shared_ptr<IDecoderFactory> decoder_factory,
                       CachingStrategy strategy)
    : file_path(path) {
    // 先探测一下文件格式
    size_t total_frames_probe = 0;
    decoder_factory->probe(this->file_path, this->format, total_frames_probe);
    switch (strategy) {
        case CachingStrategy::CACHY: {
            decoder = CachyDecoder::create(path, thread_pool, decoder_factory);
            break;
        }
        case CachingStrategy::STREAMING: {
            decoder =
                StreamingDecoder::create(path, thread_pool, decoder_factory);
            break;
        }
    }
}

}  // namespace ice
