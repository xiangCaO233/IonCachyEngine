#include "ice/config/config.hpp"
#include "ice/core/SourceNode.hpp"
#include "ice/manage/AudioBuffer.hpp"
#include "ice/manage/AudioTrack.hpp"
#include "ice/manage/dec/IDecoderFactory.hpp"
#include "ice/manage/dec/IDecoderInstance.hpp"
#include "ice/thread/ThreadPool.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <memory>
#include <semaphore>
#include <string_view>
#include <thread>

namespace
{
/// @brief 测试按引擎页边界构造估值与实际尾部。
constexpr std::size_t PAGE_FRAMES = 16384U;

/// @brief 为不同帧和声道生成可校验的非零样本。
/// @param frame 用绝对帧检测跨页拼接及尾部偏移。
/// @param channel 不同声道使用独立偏置以检测平面顺序。
/// @return 严格为正的样本，保证缺页静音不能通过校验。
float sample(std::size_t frame, std::size_t channel)
{
    return 0.125F + static_cast<float>(frame % 251U) * 0.001F +
           static_cast<float>(channel) * 0.1F;
}

/// @brief 测试拥有的供数配置及后台读取门闩。
/// 声明在音轨前，确保工作线程退出以后才销毁门闩。
struct LengthInput {
    /// @brief 与实际可读长度独立的元信息估值，零表示未知。
    std::size_t estimate;
    /// @brief 样本生成器实际可读的排除结束帧。
    std::size_t total;
    /// @brief 只拦截首次后台预读；同步首块和后续预读无门闩。
    std::binary_semaphore prefetch{ 0 };
};

/// @brief 能独立控制实际可读长度与元信息估值的顺序解码替身。
/// 样本按当前位置即时生成，避免测试持有整首曲目的副本。
/// 首块由创建者读取，此后唯一读者是预读线程。
class LengthDecoder final : public ice::IDecoderInstance
{
public:
    /// @brief 按要求的输出格式生成 PCM，借用测试持有的配置。
    /// @param format 解码期间固定的目标格式。
    /// @param input 共享后台门闩；寿命覆盖当前解码实例。
    LengthDecoder(const ice::AudioDataFormat& format, LengthInput& input)
        : m_format(format), m_input(input)
    {
    }

    /// @brief 只允许精确回到解码起点，保持正式顺序后端的约束。
    /// 测试门闩只激活一次，故定位后保留已经累计的读取次数。
    bool seek(std::size_t frame) override
    {
        if ( frame != 0 ) return false;
        m_cursor = 0;
        return true;
    }

    /// @brief 按真实长度返回 PCM，后台首次读取由测试控制。
    /// @param output 引擎准备的平面缓冲，从每个声道的第零帧开始写入。
    /// @param frames 请求容量；不得根据元信息调整真实样本生成范围。
    /// @return 本次实际有效帧数，只有末尾允许短读。
    std::size_t read(float** output, std::size_t frames) override
    {
        // 首次读取同步执行；门闩只阻挡第二次读取，便于稳定复现估值耗尽。
        if ( ++m_reads == 2U &&
             !m_input.prefetch.try_acquire_for(std::chrono::seconds(5)) )
            return 0;

        // 由真实长度裁剪最后一页，精确整页的结束通过下一次读返回零体现。
        const auto count = std::min(frames, m_input.total - m_cursor);
        for ( std::size_t channel = 0; channel < m_format.channels;
              ++channel ) {
            for ( std::size_t frame = 0; frame < count; ++frame )
                output[channel][frame] = sample(m_cursor + frame, channel);
        }
        m_cursor += count;
        return count;
    }

    /// @brief 返回固定输出格式。
    const ice::AudioDataFormat& get_source_format() const override
    {
        return m_format;
    }

    /// @brief 报告与实际供数独立的长度估计。
    std::size_t get_source_total_frames() const override
    {
        return m_input.estimate;
    }

private:
    /// @brief 实例固定输出格式。
    ice::AudioDataFormat m_format;
    /// @brief 只读取长度配置，后台仅消耗门闩的信号。
    LengthInput& m_input;
    /// @brief 顺序读游标，仅由解码器调用者串行访问。
    /// 始终小于等于实际帧数，因此按剩余长度裁剪不会下溢。
    std::size_t m_cursor{ 0U };
    /// @brief 区分同步首块和异步预读。
    std::size_t m_reads{ 0U };
};

/// @brief 同时让 AudioTrack 看到估值，并为其创建实际可供数的实例。
class LengthFactory final : public ice::IDecoderFactory
{
public:
    /// @brief 借用测试配置，保持探测报告与解码后端估值一致。
    explicit LengthFactory(LengthInput& input) : m_input(input) {}

    /// @brief 模拟容器探测通过，包括未知时长的有效音频。
    /// @param info 使用与后端估算相同的帧数，其他可选元数据保持默认。
    /// @return 总是成功，以便把测试限定在长度策略。
    bool probe(std::string_view, ice::MediaInfo& info) const override
    {
        info             = {};
        info.format      = ice::ICEConfig::internal_format;
        info.frame_count = m_input.estimate;
        return true;
    }

    /// @brief 各个实例独立推进 PCM 游标。
    /// @param format 音轨选择的格式，决定写入声道个数。
    /// @return 由音轨独占且只借用测试配置的实例。
    std::unique_ptr<ice::IDecoderInstance> create_instance(
        std::string_view, const ice::AudioDataFormat& format) const override
    {
        return std::make_unique<LengthDecoder>(format, m_input);
    }

private:
    /// @brief 测试主体保活的长度及同步配置。
    /// 工厂无需拥有 PCM，实例销毁之后测试才能释放此配置。
    LengthInput& m_input;
};

/// @brief 逐声道核对短块内容，静音填充不能作为成功。
/// @param buffer 当前调用取得的块，可能有未写入的后缀。
/// @param start 用于重建参考样本的绝对帧。
/// @param frames 需要核对的有效前缀长度。
bool matches(const ice::AudioBuffer& buffer, std::size_t start,
             std::size_t frames)
{
    for ( std::size_t channel = 0; channel < buffer.num_channels();
          ++channel ) {
        for ( std::size_t frame = 0; frame < frames; ++frame ) {
            // 优化构建可以对生成器和校验循环采用不同的乘加合并策略。
            // 容差小于相邻参考采样幅度差，仍能区分缺页静音与取错帧。
            const auto error = std::abs(buffer.raw_ptrs()[channel][frame] -
                                        sample(start + frame, channel));
            if ( !(error < 1.0e-6F) ) return false;
        }
    }
    return true;
}

/// @brief 在测试侧轮询音频页就绪，产品回调仍只尝试读取且从不等待 IO。
/// @param condition 每次都重新读缓存，静音或不完整块不能通过。
/// @return 异步读取在测试等待上限内达到指定条件时返回 true。
/// 让出调度的地方只在测试线程，后台读取不会依赖固定延迟。
template<typename Condition> bool awaitCondition(Condition condition)
{
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    do {
        if ( condition() ) return true;
        std::this_thread::yield();
    } while ( std::chrono::steady_clock::now() < deadline );
    return false;
}

/// @brief 验证耗尽估值不误停，并在真实末尾完成输出及停止。
/// @param pool 创建音轨共用的任务池，流式预读不占用此池。
/// @param estimate 短于真实数据的容器估值。
/// @param total 实际帧数，分别覆盖尾页短读和整页后的零帧读取。
/// @return 估值位置继续播放且确认末尾后精确停止时返回 true。
bool playsThroughEstimate(ice::ThreadPool& pool, std::size_t estimate,
                          std::size_t total)
{
    LengthInput input{ estimate, total };
    auto        track = ice::AudioTrack::create("stream",
                                         pool,
                                         std::make_shared<LengthFactory>(input),
                                         ice::CachingStrategy::STREAMING);
    if ( !track ) return false;

    ice::AudioBuffer block;
    if ( !block.resize(ice::ICEConfig::internal_format, 8U) ) {
        input.prefetch.release();
        return false;
    }
    ice::SourceNode source(track);
    // 测试直接对照后端生成的样本，不叠加播放器的默认音量。
    source.setvolume(1.0F);
    source.set_playpos(PAGE_FRAMES);
    source.play();

    // 后台此时拿不到第二页；播放器可临时停在估算位置，仍应保持播放状态。
    // 即使调度器尚未运行预读线程，门闩也能阻止其在以下断言前写入尾页。
    // 播放游标停在第一页后，而不是跨过未确认的帧并留下不可恢复的静音。
    source.process(block);
    const bool keptPlaying = !track->end_frame() && source.isplaying() &&
                             source.get_playpos() == PAGE_FRAMES;
    // 无论断言是否通过都解除后台门闩，保证音轨可立即析构并回收工作线程。
    input.prefetch.release();
    if ( !keptPlaying ) return false;

    // 先读跨页块，确保第二页实际播放并推动第三页预读；直接读取末页
    // 会走后台顺序跳读，无法覆盖依靠上一页调度的正常播放场景。
    // 末页读还会预取下一页，整页对齐的末尾因此可由零帧返回确认。
    if ( !awaitCondition([&] {
             return track->read(block, 2U * PAGE_FRAMES - 4U, 8U) == 8U &&
                    matches(block, 2U * PAGE_FRAMES - 4U, 8U);
         }) ||
         !awaitCondition([&] {
             return track->read(block, total - 4U, 4U) == 4U &&
                    matches(block, total - 4U, 4U) &&
                    track->end_frame() == total;
         }) )
        return false;

    // 已确认的结束帧会裁剪最后一个 8 帧请求，保留尾部 PCM 并停止播放。
    // 同时核对静音后缀及稳定停位，防止只改长度查询而未更新结束判断。
    // 预读调度与读端仍可能争用缓存锁，回调在这种情况下按约定填静音。
    // 控制侧可重新定位重试，条件始终要求同一块有末尾样本且在正确位置停止。
    return awaitCondition([&] {
               source.set_playpos(total - 4U);
               source.play();
               source.process(block);
               return matches(block, total - 4U, 4U) &&
                      block.raw_ptrs()[0][4] == 0.0F && !source.isplaying() &&
                      source.get_playpos() == total &&
                      source.num_frames() == total;
           }) &&
           track->read(block, total, 8U) == 0U;
}

/// @brief 首次短读已经确定长度，零估值不能阻止短轨及空轨建立。
/// @param total 首块能完整覆盖的真实长度；零表示空轨。
/// @return 无需释放后台门闩即可读取所有有效样本时返回 true。
bool acceptsUnknownShortFile(ice::ThreadPool& pool, std::size_t total)
{
    LengthInput input{ 0U, total };
    auto        track = ice::AudioTrack::create("stream",
                                         pool,
                                         std::make_shared<LengthFactory>(input),
                                         ice::CachingStrategy::STREAMING);
    // 首次短读使结束位置立即可用；等待后台可能掩盖初始化回归。
    // 空轨同样能够返回确定的零帧，由调用方决定它是否能加入播放队列。
    if ( !track || track->end_frame() != total ) return false;

    ice::AudioBuffer block;
    return block.resize(ice::ICEConfig::internal_format, 8U) &&
           track->read(block, 0U, 8U) == total && matches(block, 0U, total);
}
}  // namespace

/// @brief 覆盖一页估值、零估值、整页 EOF 以及短文件。
/// @return 通过为零，非零值标识各独立检查。
int main()
{
    ice::ThreadPool pool(1);
    // 分开返回各情景的状态，CTest 失败时可定位到估值或首块边界。
    if ( !playsThroughEstimate(pool, PAGE_FRAMES, 3U * PAGE_FRAMES - 31U) )
        return 1;
    // 零估值场景覆盖需向整页末端再请求一次才能确认为 EOF 的长度。
    if ( !playsThroughEstimate(pool, 0U, 3U * PAGE_FRAMES) ) return 2;
    // 空轨与真实短轨都在创建阶段确认，但只有后者可以交付样本。
    if ( !acceptsUnknownShortFile(pool, 3U) ) return 3;
    if ( !acceptsUnknownShortFile(pool, 0U) ) return 4;
    return 0;
}
