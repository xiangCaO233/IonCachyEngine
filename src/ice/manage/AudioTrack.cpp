#include <ice/manage/AudioTrack.hpp>
#include <memory>

#include "ice/config/config.hpp"
#include "ice/manage/dec/CachyDecoder.hpp"
#include "ice/manage/dec/StreamingDecoder.hpp"

namespace ice
{
/// @brief 先探测媒体元信息，再按指定策略建立音轨。
/// @warning 探测涉及文件访问，仅用于资源加载阶段。
/// 非空音轨只代表探测成功，不保证异步 PCM 解码已完成。
[[nodiscard]] std::shared_ptr<AudioTrack> AudioTrack::create(
    std::string_view path, ThreadPool& thread_pool,
    std::shared_ptr<IDecoderFactory> decoder_factory, CachingStrategy strategy)
{
    // 探测失败时直接返回空句柄，不构造缺少有效媒体信息的音轨。
    // 元信息来自探测阶段；PCM 实际帧数应另行向解码器查询。
    // 此处不缓存 probe 失败原因，失败只由空句柄向上传递。
    MediaInfo info;
    if ( !decoder_factory->probe(path, info) ) {
        return nullptr;
    }
    // 音轨会跨播放节点共享；元信息在此复制并归音轨所有。
    return std::shared_ptr<AudioTrack>(
        new AudioTrack(path, thread_pool, decoder_factory, strategy, info));
};

/// @brief 保存路径及元信息，并创建唯一拥有的解码策略。
/// 缓存策略在构造时确定，后续全局默认值变更不替换既有解码器。
/// @warning 构造可分配并提交后台任务，不能从音频回调触发。
AudioTrack::AudioTrack(std::string_view path, ThreadPool& thread_pool,
                       std::shared_ptr<IDecoderFactory> decoder_factory,
                       CachingStrategy strategy, const MediaInfo& info)
    : file_path(path), media_info(info)
{

    // 目标格式在此从全局配置取快照；播放期间修改配置不会重采样已有缓存。
    // 这里只处理已定义枚举值，调用方不可传入越界转换得到的策略。
    // 音轨不维护播放游标，切换播放位置不会重新构造此策略。
    // 策略对象独占持有，而不同播放节点共享整个音轨的生命周期。
    switch ( strategy ) {
    case CachingStrategy::CACHY: {
        // 完全缓存策略先返回任务句柄，首次查询帧数可能仍需等待。
        // 应由资源准备流程等待就绪，再发布给实时播放节点。
        decoder = CachyDecoder::create(path,
                                       ice::ICEConfig::internal_format,
                                       thread_pool,
                                       decoder_factory);
        // 解码失败会在策略首次消费结果时折叠为空缓存。
        // 因此探测成功与可读取音频是两个独立的就绪条件。
        break;
    }
    case CachingStrategy::STREAMING: {
        // 当前流式策略是占位实现，创建成功不意味着支持流式播放。
        // 保留策略分支，但消费方仍须以实际帧数判断有无数据。
        decoder = StreamingDecoder::create(path,
                                           ice::ICEConfig::internal_format,
                                           thread_pool,
                                           decoder_factory);
        break;
    }
    }
}

}  // namespace ice
