#include "ice/config/config.hpp"
#include "ice/core/SourceNode.hpp"
#include "ice/manage/AudioBuffer.hpp"
#include "ice/manage/AudioTrack.hpp"
#include "ice/manage/dec/IDecoderFactory.hpp"
#include "ice/manage/dec/IDecoderInstance.hpp"
#include "ice/thread/ThreadPool.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <memory>
#include <optional>
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
    /// @brief 开启后按目标帧精确寻址，模拟能直接按 PCM 帧定位的后端。
    bool exactSeek{};
    /// @brief 单次供数上限，小于页容量时模拟成功的有限读。
    std::size_t maxRead{ PAGE_FRAMES };
    /// @brief 请求回退后注入无法回到已知起点的错误。
    std::atomic<bool> failSeek{};
    /// @brief 精确和普通回跳次数，用于确认终止后没有再次定位。
    std::atomic<size_t> failedSeeks{};
    /// @brief 工作线程统计实际供数，测试读取只用于核对解码工作量。
    std::atomic<size_t> decodedFrames{};
    /// @brief 仅统计非顺序定位，与正常首块读取区分。
    std::atomic<size_t> exactSeeks{};
    /// @brief 在指定读操作输出有效前缀后报告底层 IO 错误。
    std::size_t failingRead{};
    /// @brief 在测试观察到正常曲尾后，注入后续回跳读取失败。
    std::atomic<bool> failAfterEof{};
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
        // 普通定位只承诺可用起点，覆盖没有精确定位能力的媒体后端。
        if ( frame != 0 ) return false;
        // 仅运行中显式开启此标志，首次创建音轨时寻址仍然应当成功。
        if ( m_input.failSeek.load(std::memory_order_relaxed) ) {
            m_input.failedSeeks.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        m_cursor = 0;
        return true;
    }

    /// @brief 模拟逐帧定位，不遍历任何中间 PCM。
    std::optional<bool> seek_exact(size_t frame) override
    {
        // 空值表示不支持当前位置，false 才表示已尝试但没有成功。
        // 分出两类结果可验证失败时确实调到从头复位的另一条路径。
        if ( !m_input.exactSeek || frame >= m_input.total ) return std::nullopt;
        // 失败只增加诊断计数，模拟游标未知而非已经完成位置变更。
        if ( m_input.failSeek.load(std::memory_order_relaxed) ) {
            m_input.failedSeeks.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        m_cursor = frame;
        m_input.exactSeeks.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    /// @brief 按真实长度返回 PCM，后台首次读取由测试控制。
    /// @param output 引擎准备的平面缓冲，从每个声道的第零帧开始写入。
    /// @param frames 请求容量；不得根据元信息调整真实样本生成范围。
    /// @return 最多 maxRead 帧，直到末尾才返回零；错误另经诊断方法提供。
    std::size_t read(float** output, std::size_t frames) override
    {
        // 错误注入依据实际后端调用次数，多个有限读合成一页时仍可区分。
        ++m_reads;
        // 首块可由多次调用组成；门闩只拦住第一次从第二页开始的读取。
        if ( !m_waitedForPrefetch && m_cursor >= PAGE_FRAMES ) {
            // 置位发生在等待前，之后的顺序读、回跳和空读都不能重复阻塞。
            m_waitedForPrefetch = true;
            // 超时仅避免测试环境中失配的门闩卡住线程回收。
            if ( !m_input.prefetch.try_acquire_for(std::chrono::seconds(5)) )
                return 0;
        }

        // 该开关在预读线程确认曲尾后才会发布，独立于首次读取的故障计数。
        if ( m_input.failAfterEof.load(std::memory_order_relaxed) ) {
            m_error = -EIO;
            return 0;
        }
        // 由真实长度裁剪最后一页，精确整页的结束通过下一次读返回零体现。
        // 既遵守调用者容量也遵守测试供给上限，有限读可在曲中任何位置发生。
        const auto count =
            std::min({ frames, m_input.total - m_cursor, m_input.maxRead });
        // 同一个绝对帧在不同调用划分下产出相同，便于确认拼接位置。
        for ( std::size_t channel = 0; channel < m_format.channels;
              ++channel ) {
            for ( std::size_t frame = 0; frame < count; ++frame )
                output[channel][frame] = sample(m_cursor + frame, channel);
        }
        // 统计后端读取实际交付的帧，兼容页尾短读和探测后的 EOF。
        m_input.decodedFrames.fetch_add(count, std::memory_order_relaxed);
        m_cursor += count;
        // 帧数与诊断独立。允许测试后端同时交付有效数据并报告错误。
        m_error = m_reads == m_input.failingRead ? -EIO : 0;
        return count;
    }

    /// @brief 最近调用报告的诊断，与设备读写线程隔离。
    int last_read_error() const noexcept override { return m_error; }

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
    /// @brief 仅第一次读到第二页时同步测试门闩，后续回跳不再等待。
    bool m_waitedForPrefetch{};
    /// @brief 与该实例最近一次 read 同步更新，不需要原子。
    int m_error{};
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
    // 多声道分别比较，避免写入指针偏移正确但只有首个平面同步。
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
    // 无有效帧时匹配为空，这一分支只由显式空轨场景使用。
    return true;
}

/// @brief 在测试侧轮询音频页就绪，产品回调仍只尝试读取且从不等待 IO。
/// @param condition 每次都重新读缓存，静音或不完整块不能通过。
/// @return 异步读取在测试等待上限内达到指定条件时返回 true。
/// 让出调度的地方只在测试线程，后台读取不会依赖固定延迟。
template<typename Condition> bool awaitCondition(Condition condition)
{
    // 只轮询可观测条件，不以等待固定时长隐式假定文件线程已经就绪。
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
/// @param maximumRead 后端单次能返回的最大帧数，独立于页容量。
/// @return 估值位置继续播放且确认末尾后精确停止时返回 true。
bool playsThroughEstimate(ice::ThreadPool& pool, std::size_t estimate,
                          std::size_t total, size_t maximumRead = PAGE_FRAMES)
{
    LengthInput input{ estimate, total };
    // 估值、真实曲长和读取上限相互独立，可检验反复短读不会改写估值。
    input.maxRead = maximumRead;
    auto track = ice::AudioTrack::create("stream",
                                         pool,
                                         std::make_shared<LengthFactory>(input),
                                         ice::CachingStrategy::STREAMING);
    if ( !track ) return false;

    // 八帧读块可以跨页且远小于单页，便于定位填充边界。
    ice::AudioBuffer block;
    if ( !block.resize(ice::ICEConfig::internal_format, 8U) ) {
        // 初始缓冲失败也要放开后台线程，避免阻塞 track 的正常析构。
        input.prefetch.release();
        return false;
    }
    ice::SourceNode source(track);
    // 测试直接对照后端生成的样本，不叠加播放器的默认音量。
    source.setvolume(1.0F);
    // 直接推进到估值边界，确定缺页行为不会被首页已缓存样本遮蔽。
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
    // 仍在播放和位置未修改两项都成立才属于正确的后台等待。
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

/// @brief 首页读到无错零帧可确定长度，零估值不妨碍短轨和空轨创建。
/// @param maximumRead 用来模拟首页在真正曲尾之前连续多次交付有效前缀。
/// @param total 首块能完整覆盖的真实长度；零表示空轨。
/// @return 无需释放后台门闩即可读取所有有效样本时返回 true。
bool acceptsUnknownShortFile(ice::ThreadPool& pool, std::size_t total,
                             size_t maximumRead = PAGE_FRAMES)
{
    LengthInput input{ 0U, total };
    // 连续受限读取累计到零帧才结束，即使估值是零也不能提前关闭首块。
    input.maxRead = maximumRead;
    auto track = ice::AudioTrack::create("stream",
                                         pool,
                                         std::make_shared<LengthFactory>(input),
                                         ice::CachingStrategy::STREAMING);
    // 首次短读使结束位置立即可用；等待后台可能掩盖初始化回归。
    // 空轨同样能够返回确定的零帧，由调用方决定它是否能加入播放队列。
    if ( !track || track->end_frame() != total ) return false;

    // 回读样本而非仅比较长度，能发现累积写入一直覆盖声道开头的错误。
    ice::AudioBuffer block;
    return block.resize(ice::ICEConfig::internal_format, 8U) &&
           track->read(block, 0U, 8U) == total && matches(block, 0U, total);
}
/// @brief 验证大跨度前跳和回跳，在首目标页就恢复正确样本。
/// 曲长以合成帧计算，不产生大文件；页数限制直接检测是否仍在顺序追赶。
/// @param pool 音轨创建共用任务池，模拟的流式后端另有预读线程。
bool seeksDistantPages(ice::ThreadPool& pool, size_t maximumRead = PAGE_FRAMES)
{
    // 足够长的合成轨让跳过一整段音频的回退方案明显超出预期工作量。
    constexpr auto pages = 400U;
    LengthInput    input{ pages * PAGE_FRAMES, pages * PAGE_FRAMES };
    input.exactSeek = true;
    // 同样的跨页目标在不同非零读取上限下应当得到完全相同的样本。
    input.maxRead = maximumRead;
    auto track = ice::AudioTrack::create("long-stream",
                                         pool,
                                         std::make_shared<LengthFactory>(input),
                                         ice::CachingStrategy::STREAMING);
    // 缓存未提供目标页时无需等待 IO，也不推进静音。先准备输出，再放开后台门闩。
    ice::AudioBuffer block;
    if ( !track || !block.resize(ice::ICEConfig::internal_format, 8U) ) {
        input.prefetch.release();
        return false;
    }
    // 两个位置都落在页中部，检验精确定位后还能正确处理缓存内偏移。
    constexpr auto forward         = 350U * PAGE_FRAMES + 37U;
    constexpr auto backward        = 25U * PAGE_FRAMES + 19U;
    const auto     before          = std::chrono::steady_clock::now();
    const bool     stalledAtTarget = track->read(block, forward, 8U) == 0U;
    // 先记录缺页返回，再允许实际解码，保证目标页此前没有被偶然填满。
    input.prefetch.release();
    if ( !stalledAtTarget ) return false;

    // 读回结果直接和绝对帧的生成器比对，不能把关键帧附近的 PCM 当作目标。
    for ( const auto position : { forward, backward } ) {
        if ( !awaitCondition([&] {
                 return track->read(block, position, 8U) == 8U &&
                        matches(block, position, 8U);
             }) )
            return false;
    }
    // 包含首块、后台预读及两次目标跳转，最多允许十页；原先需读数百页。
    // 合成样本生成次数包含短读保持的数据，不按接口调用数推断工作量。
    const auto totalDecoded =
        input.decodedFrames.load(std::memory_order_relaxed);
    return input.exactSeeks.load(std::memory_order_relaxed) >= 2U &&
           totalDecoded <= 10U * PAGE_FRAMES &&
           std::chrono::steady_clock::now() - before < std::chrono::seconds(5);
}
/// @brief 区分首读失败、后台短读失败和正常曲尾。
bool preservesReadFailure(ice::ThreadPool& pool)
{
    // 初始化路径必须拒绝整条轨，不能发布带错误的首块。
    LengthInput first{ 0U, 3U };
    first.failingRead = 1U;
    if ( ice::AudioTrack::create("broken-first",
                                 pool,
                                 std::make_shared<LengthFactory>(first),
                                 ice::CachingStrategy::STREAMING) )
        return false;

    // 另一次失败发生在第一页已发布后，此时有效的三帧可以被播放。
    LengthInput second{ PAGE_FRAMES, PAGE_FRAMES + 3U };
    second.failingRead = 2U;
    auto track =
        ice::AudioTrack::create("broken-second",
                                pool,
                                std::make_shared<LengthFactory>(second),
                                ice::CachingStrategy::STREAMING);
    // 放开后端后等待明确失败状态，不能把暂时缺页误当作故障。
    second.prefetch.release();
    // 等待明确的读取错误；拒绝把只观察到未初始化当作成功。
    if ( !track || !awaitCondition([&] {
             return track->streaming_decode_error() == -EIO;
         }) ||
         track->end_frame() )
        return false;

    struct Completion final : ice::PlayCallBack {
        /// @brief 出错的前缀结束不能被记录成正常播放完成。
        mutable int count{};
        void        play_done(bool) const override { ++count; }
        void        frameplaypos_updated(size_t) override {}
        void        timeplaypos_updated(std::chrono::nanoseconds) override {}
    };
    // 完成回调由真实 SourceNode 发送，错误路径不能伪造一个普通的结束事件。
    auto            callback = std::make_shared<Completion>();
    ice::SourceNode source(track);
    source.add_playcallback(callback);
    source.setvolume(1.0F);
    // 循环模式使错误后的 EOF 混淆尤其明显：旧实现可能会直接回跳。
    source.setloop(true);
    source.set_playpos(PAGE_FRAMES);
    source.play();
    ice::AudioBuffer output;
    if ( !output.resize(ice::ICEConfig::internal_format, 8U) ) return false;
    // 即使开启循环也应保留这三帧并停在错误位置，不能跳回帧零。
    source.process(output);
    return matches(output, PAGE_FRAMES, 3) && !source.isplaying() &&
           source.get_playpos() == PAGE_FRAMES + 3U && callback->count == 0;
}
/// @brief 已知正常曲尾不应掩盖回跳到未缓存区域后的后端错误。
bool stopsAfterFailedRewind(ice::ThreadPool& pool)
{
    // 元信息只为一页预算，实际尾页到达后会淘汰最早的缓存页。
    constexpr size_t tail = 4U * PAGE_FRAMES;
    LengthInput      input{ PAGE_FRAMES, tail + 3U };
    input.exactSeek = true;
    auto track = ice::AudioTrack::create("rewind-failure",
                                         pool,
                                         std::make_shared<LengthFactory>(input),
                                         ice::CachingStrategy::STREAMING);
    // 给后端完成初始预读的机会，后续失败只针对已经确认 EOF 的旧页。
    input.prefetch.release();
    ice::AudioBuffer block;
    if ( !track || !block.resize(ice::ICEConfig::internal_format, 8U) )
        return false;
    // 请求远处尾页，一边确认长度一边挤出最早驻留的首页。
    if ( !awaitCondition([&] {
             track->read(block, tail, 8U);
             return track->end_frame() == input.total;
         }) )
        return false;

    // 正常曲尾已经被确认。回跳重新读取是另一项操作，可能遇到真实 IO 错误。
    // 此处只注入读取故障，和下面两个定位入口均失败的场景独立。
    input.failAfterEof.store(true, std::memory_order_relaxed);
    ice::SourceNode source(track);
    source.play();
    if ( !awaitCondition([&] {
             source.process(block);
             return track->streaming_decode_error() == -EIO;
         }) )
        return false;
    // 第一次缺页可能发生在错误公开之前，再处理一块以观察终止决策。
    source.process(block);
    return !source.isplaying() && source.get_playpos() == 0;
}
/// @brief 页缓存淘汰后同时拒绝两种回跳，并检查音源不发正常完成事件。
bool rejectsUnrecoverableSeek(ice::ThreadPool& pool)
{
    // 特意远离首页与相邻预读区，保证回跳前缓存已走过淘汰循环。
    constexpr size_t tail = 6U * PAGE_FRAMES;
    LengthInput      input{ PAGE_FRAMES, tail + 5 };
    input.exactSeek = true;
    auto track = ice::AudioTrack::create("unseekable",
                                         pool,
                                         std::make_shared<LengthFactory>(input),
                                         ice::CachingStrategy::STREAMING);
    // 两页缓存读到尾页后，首页必被后续发布淘汰。
    input.prefetch.release();
    ice::AudioBuffer block;
    // 两个条件同时需要：真实尾页已得到确认，且创建时已有可用首页。
    if ( !track || !block.resize(ice::ICEConfig::internal_format, 8U) ||
         !awaitCondition([&] {
             track->read(block, tail, 8U);
             return track->end_frame() == input.total;
         }) )
        return false;

    // 注册最小监听器，只统计错误路径误发的正常完成。
    struct Completion final : ice::PlayCallBack {
        mutable int count{};  ///< 正常完成次数，定位失败时必须保持零。
        void        play_done(bool) const override { ++count; }
        void        frameplaypos_updated(size_t) override {}
        void        timeplaypos_updated(std::chrono::nanoseconds) override {}
    };
    auto            callback = std::make_shared<Completion>();
    ice::SourceNode source(track);
    source.add_playcallback(callback);
    // 精确定位和从头恢复均失败；让播放节点走真实缺页请求。
    // 失败开关在音轨已成功创建和播过尾块后才打开，隔离启动检查。
    input.failSeek.store(true, std::memory_order_relaxed);
    source.play();
    // 处理真实设备块会再次请求首页，只有明确定位错误才能终止等待。
    if ( !awaitCondition([&] {
             source.process(block);
             return track->streaming_decode_error() == -ESPIPE;
         }) )
        return false;

    // 即使重新请求相同位置，错误状态也会阻止后台再次定位。
    // 处理器停止后也从控制侧请求已缺失范围，证明请求进入门禁而非只停止音源。
    for ( int blockIndex = 0; blockIndex < 8; ++blockIndex ) {
        source.process(block);
        // 直接请求轨道可排除仅靠 SourceNode 停止造成的表面通过。
        track->read(block, 0U, 8U);
    }
    // 两次尝试分别对应精确回跳及已知起点复位，之后的读取不能增加计数。
    return !source.isplaying() && source.get_playpos() == 0 &&
           callback->count == 0 &&
           input.failedSeeks.load(std::memory_order_relaxed) == 2U;
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
    // 大幅度往返不应依赖已受验证的短文件顺序分支。
    if ( !seeksDistantPages(pool) ) return 5;
    if ( !preservesReadFailure(pool) ) return 6;
    if ( !stopsAfterFailedRewind(pool) ) return 7;
    // 每次 17 帧覆盖首块、后台跨页与精确跳页。
    // 首块请求内多次成功读应拼出完整的七帧，即使容器没有提供估值。
    if ( !acceptsUnknownShortFile(pool, 7U, 2U) ) return 8;
    // 通常走后台连续读的三页音轨，按样本检查跨页拼接及末页曲尾。
    if ( !playsThroughEstimate(pool, PAGE_FRAMES, 3U * PAGE_FRAMES - 31U, 17U) )
        return 9;
    // 同一短读契约在精确跳页后也必须成立，不能只修顺序预读。
    if ( !seeksDistantPages(pool, 17U) ) return 10;
    // 双重定位失败后持续请求缺页，必须只有第一次定位尝试且没有正常完成通知。
    if ( !rejectsUnrecoverableSeek(pool) ) return 11;
    return 0;
}
