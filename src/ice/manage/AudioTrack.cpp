#include <cmath>
#include <ice/manage/AudioTrack.hpp>
#include <limits>
#include <memory>

#include "ice/config/config.hpp"
#include "ice/manage/AudioBuffer.hpp"
#include "ice/manage/dec/CachyDecoder.hpp"
#include "ice/manage/dec/IDecoder.hpp"
#include "ice/manage/dec/IDecoderFactory.hpp"
#include "ice/manage/dec/StreamingDecoder.hpp"

namespace ice
{
/// @brief 在完整解码类型可见处销毁策略，不向公开头暴露其实现依赖。
/// @warning 低频资源释放，流式策略析构可能等待工作线程。
AudioTrack::~AudioTrack() = default;

/// @brief 查询当前解码策略的帧数，无策略时返回零。
/// @warning 完全缓存首次查询可能等待后台任务，应在播放准备阶段完成。
size_t AudioTrack::num_frames() const
{
    // 音轨不缓存第二份长度，保留流式策略修正估算后的最新结果。
    return m_decoder ? m_decoder->num_frames() : 0;
}

/// @brief 转发区间读取，不推进节点游标或调整目标容量。
/// @param buffer 调用方已准备的目标平面缓冲。
/// @param start_frame 请求区间起点。
/// @param frame_count 请求帧数，超过目标容量时拒绝整次读取。
/// @return 底层实际返回帧数，无解码器或请求超容量时为零。
/// @warning 每个音频块调用；禁止新增分配、文件访问或阻塞，缓存须预先就绪。
size_t AudioTrack::read(AudioBuffer& buffer, size_t start_frame,
                        size_t frame_count) const
{
    // 拒绝先于解码和缓存等待，不能把过大请求交给只认识裸指针的下层。
    // 按实际容量而非活动长度验证，仍允许调用方填充当前未激活的预留块。
    if ( frame_count > buffer.frame_capacity() ) return 0;
    // 多个节点可访问同一音轨的不同区间，音轨不保存共享播放位置。
    // 仅借用缓冲的地址表与格式，不复制样本所有权，也不修改活动长度。
    return m_decoder ? m_decoder->decode(buffer.raw_ptrs(),
                                         buffer.afmt.channels,
                                         start_frame,
                                         frame_count)
                     : 0;
}

/// @brief 验证浮点帧区间后向解码策略请求借用视图。
/// @param origin_data 追加视图的容器，非法请求保持原内容。
/// @param start_frame 非负有限起点，小数向零截断。
/// @param frame_count 非负有限长度，小数向零截断。
/// @return 底层实际追加的每声道帧数，拒绝请求时为零。
/// @warning 低频分析入口，底层首次加载可等待且视图容器可扩容。
size_t AudioTrack::origin(std::vector<std::span<const float>>& origin_data,
                          double start_frame, double frame_count)
{
    // 无符号整数的上界使用精确的二次幂，不将 SIZE_MAX 转成可能向上舍入的
    // double。
    const double limit = std::ldexp(1.0, std::numeric_limits<size_t>::digits);
    // 在任何浮点到整数转换及底层调用前拒绝 NaN、无穷、负值和上界。
    // 不能只检查转换后的值，越界转换本身就不满足语言契约。
    if ( !std::isfinite(start_frame) || !std::isfinite(frame_count) ||
         start_frame < 0.0 || frame_count < 0.0 || start_frame >= limit ||
         frame_count >= limit )
        return 0;
    // 保留追加语义和原有小数截断，返回底层长度让调用方判断是否取得视图。
    return m_decoder ? m_decoder->origin(origin_data,
                                         static_cast<size_t>(start_frame),
                                         static_cast<size_t>(frame_count))
                     : 0;
}

/// @brief 先探测媒体元信息，再按指定策略建立音轨。
/// @warning 探测涉及文件访问，仅用于资源加载阶段。
/// 非空音轨只代表探测成功，不保证异步 PCM 解码已完成。
[[nodiscard]] std::shared_ptr<AudioTrack> AudioTrack::create(
    std::string_view path, ThreadPool& thread_pool,
    std::shared_ptr<IDecoderFactory> decoder_factory, CachingStrategy strategy)
{
    // 探测失败时直接返回空句柄，不构造缺少有效媒体信息的音轨。
    // 元信息来自探测阶段；PCM 实际帧数应另行向解码器查询。
    // 此处不缓存 probe 返回 false 的原因；工厂抛出的历史异常仍向上传播。
    MediaInfo info;
    // 在调用工厂前验证句柄与策略，避免将非法配置带入构造。
    if ( !decoder_factory ||
         (strategy != CachingStrategy::CACHY &&
          strategy != CachingStrategy::STREAMING) ||
         !decoder_factory->probe(path, info) ) {
        return nullptr;
    }
    // 音轨会跨播放节点共享；元信息在此复制并归音轨所有。
    auto track = std::make_shared<AudioTrack>(
        CreationKey{}, path, thread_pool, decoder_factory, strategy, info);
    // 后端创建失败时不发布不可读取的音轨。
    return track->m_decoder ? std::move(track) : nullptr;
};

/// @brief 保存路径及元信息，并创建唯一拥有的解码策略。
/// 缓存策略在构造时确定，后续全局默认值变更不替换既有解码器。
/// @warning 构造可分配并提交后台任务，不能从音频回调触发。
AudioTrack::AudioTrack(CreationKey, std::string_view path,
                       ThreadPool&                      thread_pool,
                       std::shared_ptr<IDecoderFactory> decoder_factory,
                       CachingStrategy strategy, const MediaInfo& info)
    : m_mediaInfo(info), m_strategy(strategy), m_filePath(path)
{

    // 目标格式在此从全局配置取快照；播放期间修改配置不会重采样已有缓存。
    // 工厂已经验证枚举值；后端返回空 m_decoder 时由工厂转为空音轨。
    // 此处没有异常转换层，不能将所有后端失败都解释为空句柄。
    // 音轨不维护播放游标，切换播放位置不会重新构造此策略。
    // 策略对象独占持有，而不同播放节点共享整个音轨的生命周期。
    switch ( strategy ) {
    case CachingStrategy::CACHY: {
        // 完全缓存策略先返回任务句柄，首次查询帧数可能仍需等待。
        // 应由资源准备流程等待就绪，再发布给实时播放节点。
        m_decoder = CachyDecoder::create(path,
                                         ice::ICEConfig::internal_format,
                                         thread_pool,
                                         decoder_factory);
        // 解码任务的 std::exception
        // 会在首次消费时折叠为空缓存，不代表所有失败都被捕获。
        // 因此探测成功与可读取音频是两个独立的就绪条件。
        break;
    }
    case CachingStrategy::STREAMING: {
        // 流式创建同步准备首块，后续页由专用后台线程读取。
        // 未就绪页输出静音，音频回调不等待文件访问。
        m_decoder = StreamingDecoder::create(path,
                                             ice::ICEConfig::internal_format,
                                             thread_pool,
                                             decoder_factory);
        break;
    }
    }
}

}  // namespace ice
