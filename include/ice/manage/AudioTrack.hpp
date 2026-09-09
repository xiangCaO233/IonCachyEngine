#pragma once

#include <cstddef>
#include <ice/config/config.hpp>
#include <ice/manage/AudioFormat.hpp>
#include <ice/manage/dec/MediaInfo.hpp>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ice
{
class AudioBuffer;
class IDecoder;
class IDecoderFactory;

/// @brief 创建音轨时选择的解码策略，既有音轨不随全局默认值变化而切换。
enum class CachingStrategy {
    /// @brief 后台解码整个文件，准备完成后读取不可变缓存。
    CACHY,
    /// @brief 后台按需预读固定数量的 PCM 页，缺页时输出静音。
    STREAMING
};

class ThreadPool;
/// @brief 共享媒体元信息与解码策略；播放游标由消费音轨的节点维护。
class AudioTrack
{
    /// @brief 仅静态工厂能够提供的构造凭证，保留统一探测入口。
    class CreationKey
    {
        friend class AudioTrack;
        /// @brief 私有且非聚合的构造阻止外部通过空花括号生成凭证。
        CreationKey() {}

    public:
        /// @brief 标准库可转发工厂已创建的凭证，不能自行创建新凭证。
        CreationKey(const CreationKey&) = default;
    };

public:
    /// @brief 由工厂探测后构造，公开仅供 make_shared 转发内部凭证。
    AudioTrack(CreationKey, std::string_view p, ThreadPool& thread_pool,
               std::shared_ptr<IDecoderFactory> decoder_factory,
               CachingStrategy strategy, const MediaInfo& info);

    /// @brief 探测媒体并按缓存策略创建音轨，探测失败返回空句柄。
    /// @param path 媒体路径，原样复制保存，不自动转为绝对路径。
    /// @param thread_pool 缓存策略提交任务所用线程池。
    /// @param decoder_factory 探测与实际解码由同一工厂负责，空工厂返回空句柄。
    /// @param strategy 已定义的策略值；非法枚举返回空句柄。
    /// @return 探测成功后的音轨句柄，不保证已解码出可播放帧。
    /// @warning 文件探测及策略创建只允许在资源加载路径调用。
    [[nodiscard]] static std::shared_ptr<AudioTrack> create(
        std::string_view path, ThreadPool& thread_pool,
        std::shared_ptr<IDecoderFactory> decoder_factory,
        CachingStrategy strategy = ICEConfig::default_caching_strategy);

    /// @brief 释放独占解码策略，所有借用视图随之失效。
    /// @warning 资源销毁可能等待流式预读线程退出，不可在音频回调调用。
    ~AudioTrack();

    /// @brief 禁止按值复制独占解码器，调用方通过共享音轨句柄复用资源。
    AudioTrack(const AudioTrack&) = delete;
    /// @brief 禁止按值赋值，以免隐式替换既有解码策略及其借用缓存。
    AudioTrack& operator=(const AudioTrack&) = delete;

    /// @brief 返回探测阶段保存的元信息，而非实时读取进度。
    /// @return 音轨拥有的元信息引用，不可跨越音轨销毁。
    /// 源文件的格式信息不等同于内部输出格式，不能据此推断 read 的重采样结果。
    inline const MediaInfo& get_media_info() const { return m_mediaInfo; }

    /// @brief 借用创建时原样保存的路径，不承诺已经规范化或是绝对路径。
    inline const std::string& path() const { return m_filePath; }

    /// @brief 查询解码策略报告的帧数，完全缓存为实际长度，流式为可修正估算。
    /// @warning 完全缓存策略首次查询可能等待后台任务，须在播放前完成。
    size_t num_frames() const;

    /// @brief 返回创建时确定的解码策略，便于上层选择 PCM 视图或分块读取。
    CachingStrategy cachingStrategy() const noexcept { return m_strategy; }

    /// @brief 将帧区间读取转发给音轨持有的解码策略。
    /// 不会调整缓冲格式或为调用方扩容。
    /// 超过缓冲容量的请求整次拒绝，调用方按返回值处理未写入尾部。
    /// @param buffer 可写目标，验证容量但不改变活动帧数或分配存储。
    /// @param start_frame 每声道缓存起始帧，不修改音轨或调用方节点的播放游标。
    /// @param frame_count 请求帧数，超出目标可写容量时返回零。
    /// @return 解码器报告的区间帧数；额外声道及尾部可能保持原内容。
    /// 流式缺页返回值包含静音帧，不能据此断定对应 PCM 已从文件读取。
    /// @warning 音频逐块读取入口：须预先完成缓存准备，不允许新增
    /// IO、分配或阻塞。
    size_t read(AudioBuffer& buffer, size_t start_frame,
                size_t frame_count) const;
    /// @brief 向容器追加只读 PCM 视图，音轨须活过这些视图。
    /// 当前位置参数最终转换为解码器使用的整数帧索引。
    /// @param origin_data 接收借用视图的容器，原有元素不在此清理。
    /// @param start_frame 非负有限且可表示为 size_t 的起始帧，转换会截去小数。
    /// @param frame_count 非负有限且可表示为 size_t 的帧数，转换会截去小数。
    /// @return 实际追加的每声道帧数；非法范围或无可用数据时返回零。
    /// 流式策略不追加视图；离线分析需要另行加载完全缓存音轨。
    /// @warning 输出容器可扩容；该接口不适合作为实时处理的首次准备入口。
    size_t origin(std::vector<std::span<const float>>& origin_data,
                  double start_frame, double frame_count);

private:
    /// @brief 探测时复制的源媒体信息，不随后台解码进度更新。
    MediaInfo m_mediaInfo;

    /// @brief 构造后不可变，多个播放节点可以安全查询。
    CachingStrategy m_strategy;

    /// @brief 创建参数的自有字符串副本，不借用调用方 string_view。
    std::string m_filePath;

    /// @brief 音轨独占的策略对象，返回的 PCM 视图依赖其存储生命周期。
    std::unique_ptr<IDecoder> m_decoder;
};

}  // namespace ice
