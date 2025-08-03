#include <ice/manage/AudioTrack.hpp>
#include <memory>

#include "ice/config/config.hpp"
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
    // 探测文件格式
    decoder_factory->probe(file_path, media_info);

    switch (strategy) {
        case CachingStrategy::CACHY: {
            decoder =
                CachyDecoder::create(path, ice::ICEConfig::internal_format,
                                     thread_pool, decoder_factory);
            break;
        }
        case CachingStrategy::STREAMING: {
            decoder =
                StreamingDecoder::create(path, ice::ICEConfig::internal_format,
                                         thread_pool, decoder_factory);
            break;
        }
    }
}

}  // namespace ice
