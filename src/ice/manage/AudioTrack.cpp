#include <ice/manage/AudioTrack.hpp>
#include <memory>

#include "ice/config/config.hpp"
#include "ice/manage/dec/CachyDecoder.hpp"
#include "ice/manage/dec/StreamingDecoder.hpp"

namespace ice
{
// 工厂方法
[[nodiscard]] std::shared_ptr<AudioTrack> AudioTrack::create(
    std::string_view path, ThreadPool& thread_pool,
    std::shared_ptr<IDecoderFactory> decoder_factory, CachingStrategy strategy)
{
    MediaInfo info;
    if ( !decoder_factory->probe(path, info) )
        {
            return nullptr;
        }
    return std::shared_ptr<AudioTrack>(
        new AudioTrack(path, thread_pool, decoder_factory, strategy, info));
};

// 私有构造函数，强制使用工厂方法
AudioTrack::AudioTrack(std::string_view path, ThreadPool& thread_pool,
                       std::shared_ptr<IDecoderFactory> decoder_factory,
                       CachingStrategy strategy, const MediaInfo& info)
    : file_path(path), media_info(info)
{

    switch ( strategy )
        {
        case CachingStrategy::CACHY:
            {
                decoder = CachyDecoder::create(path,
                                               ice::ICEConfig::internal_format,
                                               thread_pool,
                                               decoder_factory);
                break;
            }
        case CachingStrategy::STREAMING:
            {
                decoder =
                    StreamingDecoder::create(path,
                                             ice::ICEConfig::internal_format,
                                             thread_pool,
                                             decoder_factory);
                break;
            }
        }
}

}  // namespace ice
