#include "ice/core/effect/TimeStretcher.hpp"

#include "ice/config/config.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>

namespace ice
{
namespace
{

/// @brief 单个设备 block 允许处理的最大连续区间数。
// 上限约束循环次数，不是等待数据到达的轮询窗口。
constexpr std::size_t MAX_INPUT_SEGMENTS_PER_BLOCK = 4096U;

/// @brief 将连续输入对应的精确输出帧数转换为应用层目标帧数。
/// @param exactFrames 连续流累计的浮点输出帧数。
/// @return 四舍五入并限制在 size_t 范围内的帧数。
[[nodiscard]] std::size_t roundedOutputFrames(long double exactFrames)
{
    // 连续流结束时才对累计值取整，避免每段分别取整积累尾部时长误差。
    if ( exactFrames <= 0.0L ) return 0U;
    constexpr long double MAX_FRAMES =
        static_cast<long double>(std::numeric_limits<std::size_t>::max());
    // 浮点预算不能直接越界转换成无符号帧数，达到上限时采用饱和值。
    if ( exactFrames >= MAX_FRAMES ) {
        return std::numeric_limits<std::size_t>::max();
    }
    // 调用方提供合法倍率累计的非负有限值，结果单位为每声道输出帧。
    return static_cast<std::size_t>(std::floor(exactFrames + 0.5L));
}

/// @brief 在不改变存储容量的前提下限制一次 RubberBand 输出长度。
/// @param stretcher 已预热的 RubberBand 包装。
/// @param output 固定容量输出缓冲。
/// @param outputOffset 输出起点。
/// @param input 当前连续输入。
/// @param finalInput 是否结束当前 RubberBand 流。
/// @param maxWriteFrames 本次允许写入的最大帧数。
/// @return 实际写入帧数。
/// @warning 每段音频处理调用，只调整活动长度，不得改变分配容量。
/// 调用方保证 outputOffset 不超过当前有效长度，输入与输出不重叠。
[[nodiscard]] std::size_t processWithOutputLimit(
    RStretcher& stretcher, AudioBuffer& output, std::size_t outputOffset,
    const AudioBuffer& input, bool finalInput, std::size_t maxWriteFrames)
{
    // 保存逻辑块长，缩小有效区间不改变声道指针或存储跨度。
    const std::size_t originalFrames = output.num_frames();
    const std::size_t writableFrames =
        std::min(maxWriteFrames, originalFrames - outputOffset);
    // 临时缩小可写窗口，让底层同时遵守设备容量和当前流的尾部预算。
    // 零可写长度仍允许提交输入或 final，不能因此跳过后端状态推进。
    output.set_active_frames(outputOffset + writableFrames);
    const std::size_t written =
        stretcher.process_into(output, outputOffset, input, finalInput);
    // 恢复外层块长，调用方继续在同一块内拼接后续区间。
    output.set_active_frames(originalFrames);
    return written;
}

/// @brief 在不改变存储容量的前提下限制一次 RubberBand drain 长度。
/// @param stretcher 已提交 final 的 RubberBand 包装。
/// @param output 固定容量输出缓冲。
/// @param outputOffset 输出起点。
/// @param maxWriteFrames 本次允许写入的最大帧数。
/// @return 实际写入帧数。
/// @warning 音频尾部路径调用，不等待未来数据、不扩容；偏移必须处于块内。
[[nodiscard]] std::size_t drainWithOutputLimit(RStretcher&  stretcher,
                                               AudioBuffer& output,
                                               std::size_t  outputOffset,
                                               std::size_t  maxWriteFrames)
{
    const std::size_t originalFrames = output.num_frames();
    const std::size_t writableFrames =
        std::min(maxWriteFrames, originalFrames - outputOffset);
    output.set_active_frames(outputOffset + writableFrames);
    // 只取已有尾音，不向上游拉取新输入；短读交由外层决定是否跨块继续。
    const std::size_t written = stretcher.drain_into(output, outputOffset);
    output.set_active_frames(originalFrames);
    return written;
}

}  // namespace

/// @brief 一份只在音频线程中执行、由控制线程创建和销毁的处理状态。
struct TimeStretcher::ProcessingState {
    /// @brief 构造并预热完整状态。
    /// @param format 固定音频格式。
    /// @param quality RubberBand 质量档位。
    /// @param maxOutputFrames 最大输出 block 帧数。
    /// @param initialStretchRatio 初始拉伸倍率。
    /// @param initialPitchRatio 初始音高倍率。
    /// @param playbackRatio 此状态固定消耗的输入输出帧比。
    /// @param pitchSemitones 此状态固定的半音偏移。
    /// @param generation 状态代际。
    /// @warning 控制线程构造，可分配及预热；发布后算法对象只由音频线程执行。
    ProcessingState(const AudioDataFormat& format, TimeStretchQuality quality,
                    std::size_t maxOutputFrames, double initialStretchRatio,
                    double initialPitchRatio, double playbackRatio,
                    double pitchSemitones, std::uint64_t generation)
        : maxOutputFrames(maxOutputFrames)
        , maxInputFrames(
              static_cast<std::size_t>(std::ceil(
                  static_cast<double>(maxOutputFrames) * MAX_PLAYBACK_RATIO)) +
              1U)
        , inputBuffer(format, maxInputFrames)
        , playbackRatio(playbackRatio)
        , pitchSemitones(pitchSemitones)
        , bypass(should_bypass(playbackRatio, pitchSemitones))
        , generation(generation)
    {
        // 容量按最高播放倍率准备，输入小数进位额外预留一帧。
        // 输入申请失败时保留失败候选，发布入口负责拒绝，不继续申请后端资源。
        if ( inputBuffer.frame_capacity() != maxInputFrames ) return;
        // 旁路状态不创建算法后端，避免单位倍率也承担预热成本。
        if ( !bypass ) {
            stretcher = std::make_unique<RStretcher>(format,
                                                     quality,
                                                     maxInputFrames,
                                                     maxOutputFrames,
                                                     initialStretchRatio,
                                                     initialPitchRatio);
        }
        // 预先清除对齐填充区，首次音频处理不暴露未写入的样本。
        inputBuffer.clear();
    }

    /// @brief 最大输出 block 帧数。
    /// 这是设备回调容量契约，不是连续流的总长度。
    std::size_t maxOutputFrames{ 0U };

    /// @brief 为最高播放速度预分配的最大输入帧数。
    /// 每段请求不得超出此值，超出时不能在回调中 resize。
    std::size_t maxInputFrames{ 0U };

    /// @brief 复用的上游输入缓冲。
    /// 各段只切换有效长度，共用同一存储而不保存跨段借用地址。
    AudioBuffer inputBuffer;

    /// @brief 此状态固定的播放速度倍率。
    /// 不跟随期望原子值原地变更，防止参数与后端配置分离。
    double playbackRatio{ 1.0 };

    /// @brief 此状态固定的音高偏移。
    double pitchSemitones{ 0.0 };

    /// @brief 此状态是否采用直通路径。
    /// 构造时决定，整个实例生命周期保持不变。
    bool bypass{ true };

    /// @brief 非直通状态独占的预热 RubberBand 包装。
    std::unique_ptr<RStretcher> stretcher;

    /// @brief 跨 block 保留的输入帧小数余量。
    /// 将不足一帧的请求留给下一块，而非每块截断丢弃。
    double inputFrameRemainder{ 0.0 };

    /// @brief 此状态是否已提交 final 输入。
    /// 提交与排空是两个阶段，已提交后禁止再次拉取上游。
    bool finalSubmitted{ false };

    /// @brief 此状态的 final 输出预算是否已交付完，不表示后端队列为空。
    bool finalDrained{ false };

    /// @brief 是否正在跨 block drain discontinuity 前的旧段尾音。
    /// 为真时保留后端及余量，禁止接收新参数状态。
    bool discontinuityDrainPending{ false };

    /// @brief 是否等待 epoch provider 确认已经处理的显式边界。
    /// 只防止重复 reset，不会阻塞等待外部 epoch。
    bool awaitingBoundaryEpochAcknowledgement{ false };

    /// @brief discontinuity 跨 block drain 后仍需拉取的原 block 输入帧。
    /// 保存未读取的数量计划，不保存 PCM 地址。
    std::size_t pendingInputFramesAfterBoundary{ 0U };

    /// @brief 当前连续算法流按输入倍率累计的精确目标输出帧。
    /// 新流从零开始，长双精度余量保留到段尾统一取整。
    long double exactStreamOutputFrames{ 0.0L };

    /// @brief 当前连续算法流已经交付给下游的输出帧。
    /// 不包含块中尚未被算法覆盖的静音区域。
    std::size_t deliveredStreamOutputFrames{ 0U };

    /// @brief final 或 discontinuity 尚需交付的精确输出预算。
    /// 预算限制应用层时长，不以算法内部是否仍有尾部数据作为完成条件。
    std::size_t terminalOutputFramesRemaining{ 0U };

    /// @brief 状态代际。
    std::uint64_t generation{ 0U };

    /// @brief 无锁退役链的下一节点。
    /// 入链后音频侧不再访问实例，控制侧摘链后独占回收。
    ProcessingState* nextRetired{ nullptr };
};

/// @brief 用默认格式建立初始状态，并在发布节点前接收该状态。
/// @warning 构造属于控制路径，会分配和预热，不能在音频回调中执行。
TimeStretcher::TimeStretcher()
    : m_controlFormat(ICEConfig::internal_format)
    , m_controlMaxOutputFrames(
          std::max<std::size_t>(1U, ICEConfig::default_buffer_size))
{
    publish_prepared_state(m_controlFormat,
                           m_controlMaxOutputFrames,
                           m_quality.load(std::memory_order_relaxed));
    // 此时节点尚未被音频线程访问，可以直接安装初始状态。
    apply_pending_state();
}

/// @brief 在音频线程停止访问后回收活动、待接收及退役状态。
/// @warning 析构会释放后端资源，调用方必须先停止 process 和所有控制写入。
TimeStretcher::~TimeStretcher()
{
    collect_retired_states();

    // 各槽拥有不同状态；待接收状态尚未成为音频线程当前实例。
    std::unique_ptr<ProcessingState> pending{ m_pendingState.exchange(
        nullptr, std::memory_order_acq_rel) };
    std::unique_ptr<ProcessingState> current{ m_currentState };
    m_currentState = nullptr;

    collect_retired_states();
}

/// @brief 在一个输出块内协调参数接收、连续段拉取和尾音交付。
/// @param buffer 已准备的固定格式输出块，处理失败或未填满部分保持静音。
/// @warning 每音频块调用，只允许单音频线程访问当前状态，禁止分配、锁和销毁。
/// 控制参数通过原子邮箱传递，音频侧只在允许的边界接收完整预热状态。
void TimeStretcher::process(AudioBuffer& buffer)
{
    // 暂停优先于所有邮箱消费：不拉输入、不排尾音，也不应用待切换参数。
    // acquire 观察控制侧发布的暂停要求，恢复后继续既有算法历史。
    if ( m_paused.load(std::memory_order_acquire) ) {
        buffer.clear();
        return;
    }

    // 完整状态替换只发生在块入口，尾音未交付完时会保留旧状态。
    apply_pending_state();

    // 上游所有权由基类维持，回调中只借用，不能并发替换输入节点。
    IAudioNode* inputNode = get_inputnode_observer();
    // 缺少上游时尚未消费 final 或定位请求，不在此构建音频图。
    if ( !inputNode || !m_currentState ) {
        buffer.clear();
        return;
    }

    // 旁路仍需消费代际和结束标志，但没有 RubberBand 历史需要重置。
    const bool resetStretcher = !m_currentState->bypass;
    // 显式边界旧段仍在排尾音时，不先读取 epoch 去清掉这段历史。
    if ( !m_currentState->discontinuityDrainPending ) {
        // 显式边界已重置后，随后到达的同次 epoch 只确认代际，避免二次清后端。
        const bool suppressProviderReset =
            m_currentState->awaitingBoundaryEpochAcknowledgement;
        const bool providerChanged = apply_provider_discontinuity(
            resetStretcher && !suppressProviderReset);
        // 只有观察到变化才解除确认标志，重复的同一 epoch 不触发。
        if ( suppressProviderReset && providerChanged ) {
            m_currentState->awaitingBoundaryEpochAcknowledgement = false;
        }
    }
    // 手动定位请求独立于 provider，可主动放弃旧历史而不是等待其排空。
    apply_discontinuity_request(resetStretcher);

    // 终止态持续静音；单纯切参数不得自动重开已经结束的上游。
    if ( m_currentState->finalSubmitted && m_currentState->finalDrained ) {
        buffer.clear();
        return;
    }

    // 热路径只拒绝不匹配格式或超容量块，不能临时调整算法及存储规模。
    // 计数同时包含格式拒绝，不应仅据名称将其解释为分配不足。
    if ( buffer.afmt != m_currentState->inputBuffer.afmt ||
         buffer.num_frames() > m_currentState->maxOutputFrames ) {
        m_capacityOverflowCount.fetch_add(1U, std::memory_order_relaxed);
        buffer.clear();
        return;
    }

    // final 提交后只交付剩余预算，不再向上游请求一整块静音来驱动算法。
    if ( m_currentState->finalSubmitted && !m_currentState->bypass ) {
        if ( !m_currentState->finalDrained ) {
            buffer.clear();
            // 整块先清零再覆盖尾音，短读不会留下上一回调的音频。
            const std::size_t written = drainWithOutputLimit(
                *m_currentState->stretcher,
                buffer,
                0U,
                m_currentState->terminalOutputFramesRemaining);
            // 消耗的是实际写出帧数，短读不等于剩余预算全部完成。
            m_currentState->deliveredStreamOutputFrames += written;
            // helper 限制 written 不超过预算，减法不会从零回绕。
            m_currentState->terminalOutputFramesRemaining -= written;
            m_currentState->finalDrained =
                m_currentState->terminalOutputFramesRemaining == 0U;
            // release 向控制侧发布完成状态，显示层不用读取音频私有计数器。
            m_finalInputDrained.store(m_currentState->finalDrained,
                                      std::memory_order_release);
        } else {
            buffer.clear();
        }
        return;
    }

    const std::size_t outputFrames = buffer.num_frames();
    // 零长度块不产生输入请求，也不做后续输入/输出比值除法。
    if ( outputFrames == 0U ) return;

    buffer.clear();
    // 输出前缀可能先被上一连续段的尾音占据，新段只使用余下窗口。
    std::size_t initialOutputOffset = 0U;
    if ( m_currentState->discontinuityDrainPending ) {
        initialOutputOffset =
            drainWithOutputLimit(*m_currentState->stretcher,
                                 buffer,
                                 0U,
                                 m_currentState->terminalOutputFramesRemaining);
        m_currentState->deliveredStreamOutputFrames += initialOutputOffset;
        m_currentState->terminalOutputFramesRemaining -= initialOutputOffset;
        // 本块不足以交付旧段时立即返回，保留下一块继续 drain 的状态。
        if ( m_currentState->terminalOutputFramesRemaining > 0U ) {
            m_actualPlaybackRatio.store(0.0, std::memory_order_relaxed);
            return;
        }

        // 重置会清掉段间记账，先保存先前已计划但尚未消费的新段输入。
        const std::size_t carriedInputFrames =
            m_currentState->pendingInputFramesAfterBoundary;
        reset_processing_history(true);
        // 恢复旧计划，不能按新块空间重复计算同一份待消费输入。
        m_currentState->pendingInputFramesAfterBoundary = carriedInputFrames;
        m_currentState->awaitingBoundaryEpochAcknowledgement = true;
        // 旧尾音恰好填满块时不拉新输入，计划的余量继续跨块保留。
        if ( initialOutputOffset >= outputFrames ) {
            m_actualPlaybackRatio.store(0.0, std::memory_order_relaxed);
            return;
        }
    }

    // 输入预算以新段窗口为分母，不把旧尾音长度再次计算进去。
    const std::size_t outputFramesToFill = outputFrames - initialOutputOffset;
    std::size_t       inputFrames        = 0U;
    // 跨边界遗留计划优先于新预算，避免同一份输入时间被计算两次。
    if ( m_currentState->pendingInputFramesAfterBoundary > 0U ) {
        inputFrames = m_currentState->pendingInputFramesAfterBoundary;
        // 取走即清空，后续无边界块不能重复消费这份计划。
        m_currentState->pendingInputFramesAfterBoundary = 0U;
    } else {
        // 速度定义为输入/输出帧比；小数余量跨块累计以维持长期平均倍率。
        const double requestedInputFrames =
            static_cast<double>(outputFramesToFill) *
                m_currentState->playbackRatio +
            m_currentState->inputFrameRemainder;
        inputFrames =
            static_cast<std::size_t>(std::floor(requestedInputFrames));
        // floor 消耗整数帧，差值留在 [0,1) 内供后续正常块使用。
        m_currentState->inputFrameRemainder =
            requestedInputFrames - static_cast<double>(inputFrames);
    }
    // 容量上限是构造时承诺的输入缓冲空间，不在音频线程自动扩容。
    if ( inputFrames > m_currentState->maxInputFrames ) {
        m_capacityOverflowCount.fetch_add(1U, std::memory_order_relaxed);
        buffer.clear();
        return;
    }

    // 入口已经存在的 final 请求表示本次计划输入完成后结束。
    // 拉取上游过程中才产生的新请求，分段函数会在本段之后单独识别。
    const std::uint64_t requestedFinal =
        m_requestedFinalGeneration.load(std::memory_order_acquire);
    const std::uint64_t consumedFinal =
        m_consumedFinalGeneration.load(std::memory_order_relaxed);
    // 邮箱只判断代际有无变化，不逐个重放未消费请求。
    const bool finalAtBlockEnd = requestedFinal != consumedFinal;

    // 以已激活状态选路，不按期望值切到尚未准备的后端。
    const std::size_t pulledInputFrames =
        m_currentState->bypass
            ? process_bypass_segments(buffer,
                                      *inputNode,
                                      inputFrames,
                                      finalAtBlockEnd,
                                      initialOutputOffset)
            : process_stretched_segments(buffer,
                                         *inputNode,
                                         inputFrames,
                                         finalAtBlockEnd,
                                         initialOutputOffset);
    // 此诊断比值基于请求到上游的帧数，不是后端实际出音帧数或 UI 期望倍率。
    // 纯排尾音块在前面的路径直接发布零，不能推断播放器已经暂停。
    const double actualRatio = static_cast<double>(pulledInputFrames) /
                               static_cast<double>(outputFramesToFill);
    m_actualPlaybackRatio.store(actualRatio, std::memory_order_relaxed);
}

/// @brief 校验格式和容量后，在控制侧建立可替换的完整处理状态。
/// @return 无效参数返回 false，且不覆盖此前的准备配置。
/// @warning 低频控制接口会分配和预热，必须与其他构建状态的 setter 串行。
/// @param format 内部固定声道数和采样率，不能在当前状态运行中原地更改。
/// @param maxOutputFrames 设备可请求的最大输出帧数，不是一次上游输入长度。
bool TimeStretcher::prepare(const AudioDataFormat& format,
                            std::size_t            maxOutputFrames)
{
    // 输入容量按最大速度放大并留一帧，先限制整数乘加的可表示范围。
    constexpr std::size_t MAX_RATIO_FRAMES =
        static_cast<std::size_t>(MAX_PLAYBACK_RATIO);
    if ( format.channels == 0U || format.samplerate == 0U ||
         maxOutputFrames == 0U ||
         maxOutputFrames > (std::numeric_limits<std::size_t>::max() - 1U) /
                               MAX_RATIO_FRAMES ) {
        return false;
    }

    // 只有通过校验才保存控制配置，后续参数变化沿用这组容量与格式。
    m_controlFormat = format;
    // 未来重建复用此控制配置，不共享当前音频缓冲的可变容量字段。
    m_controlMaxOutputFrames = maxOutputFrames;
    return publish_prepared_state(
        format, maxOutputFrames, m_quality.load(std::memory_order_acquire));
}

/// @brief 发布新的期望速度，并为其预热替换状态。
/// 无效倍率被忽略，不夹取到边界，也不重置此前可用的速度。
/// @warning 控制线程操作；原子参数不意味着状态构造能由多个写入者并发调用。
/// @param desiredRatio 输入帧数与输出帧数之比，必须处于已定义的速度范围。
void TimeStretcher::set_playback_ratio(double desiredRatio)
{
    if ( !std::isfinite(desiredRatio) || desiredRatio < MIN_PLAYBACK_RATIO ||
         desiredRatio > MAX_PLAYBACK_RATIO ) {
        return;
    }

    // 期望值先对查询可见，实际音频状态要等下一次允许替换的块边界。
    const double previous = m_desiredPlaybackRatio.exchange(
        desiredRatio, std::memory_order_acq_rel);
    // 相同参数不重复预热；这不是为了延迟用户参数生效而设置的时间窗口。
    if ( std::abs(previous - desiredRatio) <=
         std::numeric_limits<double>::epsilon() ) {
        return;
    }
    publish_prepared_state(m_controlFormat,
                           m_controlMaxOutputFrames,
                           m_quality.load(std::memory_order_acquire));
}

/// @brief 设置两倍八度范围内的期望音高偏移并发布新状态。
/// @warning 控制路径可分配和预热，不能从上游音频节点或回调中调用。
/// @param semitones 相对原始音高的半音偏移，十二半音对应倍率翻倍。
void TimeStretcher::set_pitch_semitones(double semitones)
{
    // 非有限数和范围外值保持旧设置，避免对非法参数执行指数换算。
    if ( !std::isfinite(semitones) || semitones < -24.0 || semitones > 24.0 ) {
        return;
    }

    const double previous =
        m_desiredPitchSemitones.exchange(semitones, std::memory_order_acq_rel);
    // 这里只判断参数是否变化，不直接修改运行中后端的音高状态。
    if ( std::abs(previous - semitones) <=
         std::numeric_limits<double>::epsilon() ) {
        return;
    }
    publish_prepared_state(m_controlFormat,
                           m_controlMaxOutputFrames,
                           m_quality.load(std::memory_order_acquire));
}

/// @brief 查询最近发布的输入/输出帧比诊断值。
/// @warning 音频侧更新、控制侧读取，relaxed 只提供标量观测，不同步 PCM。
double TimeStretcher::get_actual_playback_ratio() const
{
    return m_actualPlaybackRatio.load(std::memory_order_relaxed);
}

/// @brief 返回控制侧期望音高，不证明相应预热状态已经在音频侧激活。
double TimeStretcher::get_pitch_semitones() const
{
    return m_desiredPitchSemitones.load(std::memory_order_relaxed);
}

/// @brief 质量变化时重建后端，不在现有算法流内动态改变档位。
/// @warning 低频控制接口，必须与 prepare 和其他参数 setter 串行。
/// @param quality 新后端的质量档位；旧后端在切换前继续使用旧配置。
void TimeStretcher::set_quality(TimeStretchQuality quality)
{
    const TimeStretchQuality previous =
        m_quality.exchange(quality, std::memory_order_acq_rel);
    // 相同质量无需发布另一代状态，避免重复初始化相同后端。
    if ( previous == quality ) return;

    publish_prepared_state(m_controlFormat, m_controlMaxOutputFrames, quality);
}

/// @brief 获取期望质量；当前音频状态可能仍在排完旧参数的尾部。
TimeStretchQuality TimeStretcher::get_quality() const
{
    // 此处读的是期望档位，尾音交付期间仍可能在使用旧质量的处理状态。
    return m_quality.load(std::memory_order_acquire);
}

/// @brief 向音频线程发布暂停要求，不销毁或重置算法历史。
/// @warning 控制侧 release 写入、音频侧 acquire 读取，不等待回调确认。
/// @param paused 为真时仅要求后续回调静音，不等待正在执行的块结束。
void TimeStretcher::set_paused(bool paused) noexcept
{
    m_paused.store(paused, std::memory_order_release);
}

/// @brief 查询暂停要求，不代表音频线程已经结束当前正在执行的块。
bool TimeStretcher::is_paused() const noexcept
{
    return m_paused.load(std::memory_order_acquire);
}

/// @brief 发布新定位代际，允许音频侧在安全点丢弃旧算法历史。
/// @warning 控制侧 release 递增、音频侧 acquire 消费，不直接执行后端 reset。
std::uint64_t TimeStretcher::request_discontinuity() noexcept
{
    // 返回递增后的代际，控制方可与 consumed 查询比较是否已消费。
    return m_requestedDiscontinuityGeneration.fetch_add(
               1U, std::memory_order_release) +
           1U;
}

/// @brief 获取音频侧最近消费的定位请求代际，多个未消费请求可合并。
std::uint64_t TimeStretcher::consumed_discontinuity_generation() const
{
    return m_consumedDiscontinuityGeneration.load(std::memory_order_acquire);
}

/// @brief 用序号包围 context 与 reader 的更新，供音频侧检查配置是否变化。
/// @warning 配置写入者必须串行，旧 context 须保持有效直到所有回调停止。
/// @param context 借用的代际来源，配置清除后也不能立即假定没有旧读者。
/// @param reader 返回当前 epoch 的函数，不应分配、等待或触发状态构建。
void TimeStretcher::set_discontinuity_generation_provider(
    const void* context, DiscontinuityGenerationReader reader)
{
    // 奇数表示更新进行中，音频侧看到该值就放弃本次读取而非自旋等待。
    m_providerConfigurationSequence.fetch_add(1U, std::memory_order_acq_rel);
    // 此处仅发布地址，不复制或析构 provider，因此外部必须负责对象存活。
    m_discontinuityProviderContext.store(context, std::memory_order_relaxed);
    m_discontinuityGenerationReader.store(reader, std::memory_order_relaxed);
    // 最后恢复偶数序号；该协议只描述配置发布，不接管外部上下文所有权。
    m_providerConfigurationSequence.fetch_add(1U, std::memory_order_release);
}

/// @brief 经同一配置发布入口清除 provider，手动定位邮箱保持独立。
/// @warning 清除不等待正在执行的旧 reader，也不授权立即销毁其 context。
void TimeStretcher::clear_discontinuity_generation_provider()
{
    set_discontinuity_generation_provider(nullptr, nullptr);
}

/// @brief 获取最后一次向控制侧发布的外部代际，清除 provider 不清零此诊断值。
std::uint64_t TimeStretcher::observed_provider_discontinuity_generation() const
{
    return m_publishedProviderGeneration.load(std::memory_order_acquire);
}

/// @brief 发布输入区间查询器，用于在上游拉取前切分连续音频段。
/// @warning 单控制写入者更新；函数指针不拥有上下文，reader 需无阻塞和分配。
/// @param context 区间查询器的非拥有上下文，可包含其私有的区间游标。
/// @param reader 接受剩余输入上限，返回当前连续段长度及末端动作。
void TimeStretcher::set_input_boundary_provider(void*               context,
                                                InputBoundaryReader reader)
{
    // 与 epoch provider 分开编号，两个配置不要求作为一个事务同时切换。
    m_inputBoundaryConfigurationSequence.fetch_add(1U,
                                                   std::memory_order_acq_rel);
    // 区间配置与 epoch 配置各自发布，调用方不能假定两次设置原子地同时生效。
    m_inputBoundaryProviderContext.store(context, std::memory_order_relaxed);
    // 字段自身为原子，读侧还会复查序号；这并不延长 context 的生命周期。
    m_inputBoundaryReader.store(reader, std::memory_order_relaxed);
    m_inputBoundaryConfigurationSequence.fetch_add(1U,
                                                   std::memory_order_release);
}

/// @brief 清除区间查询器，后续按剩余输入预算使用无显式边界的默认段。
/// @warning 旧 context 仍可能被正在执行的 reader 使用，不能立即回收。
void TimeStretcher::clear_input_boundary_provider()
{
    set_input_boundary_provider(nullptr, nullptr);
}

/// @brief 请求当前输入流结束，并返回可用于查询消费进度的代际。
/// @warning 通过原子邮箱由控制侧或上游回调发布，不等待尾部输出完成。
std::uint64_t TimeStretcher::request_final_input() noexcept
{
    // 新请求先撤销旧的完成显示；真正的终止状态由音频侧提交 final 后发布。
    m_finalInputDrained.store(false, std::memory_order_release);
    return m_requestedFinalGeneration.fetch_add(1U, std::memory_order_release) +
           1U;
}

/// @brief 查询已提交的 final 代际；请求已消费不等于尾音预算已经交付完。
std::uint64_t TimeStretcher::consumed_final_generation() const
{
    return m_consumedFinalGeneration.load(std::memory_order_acquire);
}

/// @brief 查询应用层要求交付的 final 帧预算是否完成，旁路可立即完成。
/// 此标志不暴露后端队列内部是否还残留超出目标时长的采样。
bool TimeStretcher::is_final_input_drained() const
{
    // 只观察已发布标志，不通过查询进入后端 drain 或消费音频。
    return m_finalInputDrained.load(std::memory_order_acquire);
}

/// @brief 接管整条已退役链并在控制侧销毁，避免音频侧释放后端内存。
/// @warning 只能在低频控制路径执行，会析构算法状态，不可与其他控制操作并发。
void TimeStretcher::collect_retired_states()
{
    // exchange 一次摘下当前链；此后音频线程追加的节点留给下次回收。
    ProcessingState* retired =
        m_retiredStates.exchange(nullptr, std::memory_order_acq_rel);
    while ( retired ) {
        // 必须先保存下一节点，再由局部 unique_ptr 结束当前节点生命周期。
        ProcessingState*                 next = retired->nextRetired;
        std::unique_ptr<ProcessingState> reclaim{ retired };
        retired = next;
    }
}

/// @brief 查询音频侧已安装的参数状态代际，不以 setter 返回作为生效证明。
std::uint64_t TimeStretcher::active_state_generation() const
{
    // 不触发 pending 接收，避免控制侧查询改变音频线程的状态切换时机。
    return m_activeStateGeneration.load(std::memory_order_acquire);
}

/// @brief 读取格式或容量校验失败的累计诊断计数。
/// @warning 音频侧递增、控制侧读取，relaxed 不用于传递处理数据。
std::uint64_t TimeStretcher::capacity_overflow_count() const
{
    return m_capacityOverflowCount.load(std::memory_order_relaxed);
}

/// @brief 实现基类效果钩子，向已有后端提交一块非 final 输入。
/// @warning 音频热路径只使用预热状态；分段、结束和直通语义由重载 process 负责。
/// @param output 后端写入的输出块，输入输出存储必须相互独立。
/// @param input 已经拉取的连续输入，该钩子不检测或切分其中的边界。
void TimeStretcher::apply_effect(AudioBuffer& output, const AudioBuffer& input)
{
    // 该钩子不实现旁路复制，无后端时明确静音，不能替代完整 process 流程。
    if ( !m_currentState || !m_currentState->stretcher ) {
        output.clear();
        return;
    }
    m_currentState->stretcher->process(output, input, false);
}

/// @brief 控制侧完成构造预热后，把最新状态放入单槽邮箱。
/// @warning 会分配及销毁被覆盖的未激活状态，必须由单控制线程串行调用。
/// 返回表示已发布，不保证音频线程已经接收；尾音期间允许延后替换。
/// @param format 新实例固定使用的音频格式，不能从现有块临时推断。
/// @param maxOutputFrames 新实例承诺支持的最大设备块长度。
/// @param quality 新实例预热时使用的质量，发布之后不原地更换。
bool TimeStretcher::publish_prepared_state(const AudioDataFormat& format,
                                           std::size_t        maxOutputFrames,
                                           TimeStretchQuality quality)
{
    collect_retired_states();

    const double playbackRatio =
        m_desiredPlaybackRatio.load(std::memory_order_acquire);
    const double pitchSemitones =
        m_desiredPitchSemitones.load(std::memory_order_acquire);
    // 播放速度是输入/输出比，而后端 stretch 是输出/输入比，两者互为倒数。
    const double initialStretchRatio = 1.0 / playbackRatio;
    // 半音转换为频率倍率：十二个半音对应一个八度，不改变时间倍率定义。
    const double initialPitchRatio = std::pow(2.0, pitchSemitones / 12.0);
    const std::uint64_t generation = m_nextStateGeneration++;

    // 预热在发布前完成，音频侧无需在首次 process 补做准备。
    auto prepared = std::make_unique<ProcessingState>(format,
                                                      quality,
                                                      maxOutputFrames,
                                                      initialStretchRatio,
                                                      initialPitchRatio,
                                                      playbackRatio,
                                                      pitchSemitones,
                                                      generation);
    // 构造完成不等于工作区准备成功；失败候选在控制侧回收，保留现有邮箱。
    if ( prepared->inputBuffer.frame_capacity() != prepared->maxInputFrames ||
         (!prepared->bypass &&
          (!prepared->stretcher || !prepared->stretcher->isValid())) )
        return false;
    // 完整对象构造完才让出所有权；音频线程不会看到半初始化的后端。
    ProcessingState* published = prepared.release();
    // exchange 返回的旧 pending 从未被音频侧取得，可在控制线程立即销毁。
    // 若音频侧已接收旧状态，此处返回空指针，不会与当前实例共享所有权。
    std::unique_ptr<ProcessingState> superseded{ m_pendingState.exchange(
        published, std::memory_order_acq_rel) };
    return true;
}

/// @brief 在块入口接收完整预热状态，将旧状态转入延迟回收链。
/// @warning 音频热路径，acq_rel 交换接收控制发布，不能分配或析构旧实例。
void TimeStretcher::apply_pending_state()
{
    // final 或段间尾音未交付完时继续使用旧后端，避免参数切换截断尾部。
    if ( m_currentState &&
         ((m_currentState->finalSubmitted && !m_currentState->finalDrained) ||
          m_currentState->discontinuityDrainPending) ) {
        return;
    }

    ProcessingState* pending =
        m_pendingState.exchange(nullptr, std::memory_order_acq_rel);
    // 没有新状态就复用当前历史，不重置分数帧余量。
    if ( !pending ) return;

    // 已结束的流只换参数不复活，必须通过显式历史重置开启新一段输入。
    const bool preserveTerminalState = m_currentState &&
                                       m_currentState->finalSubmitted &&
                                       m_currentState->finalDrained;
    pending->finalSubmitted          = preserveTerminalState;
    pending->finalDrained            = preserveTerminalState;

    // 当前指针由唯一音频线程使用；先完成替换，再把旧实例明确标记为可回收。
    ProcessingState* previous = m_currentState;
    m_currentState            = pending;
    m_activeStateGeneration.store(pending->generation,
                                  std::memory_order_release);
    // 激活先发布设定倍率，实际拉取后再由 process 更新观测值。
    m_actualPlaybackRatio.store(pending->playbackRatio,
                                std::memory_order_relaxed);
    m_finalInputDrained.store(preserveTerminalState, std::memory_order_release);
    retire_state(previous);
}

/// @brief 将不再使用的实例压入退役链，实际析构留给控制线程。
/// @warning 音频热路径只做指针和原子操作，不访问压栈后实例内容。
/// @param state 当前音频线程已停止使用的独占实例，空指针无需入链。
void TimeStretcher::retire_state(ProcessingState* state)
{
    // 首次安装没有旧实例，无需额外分配退役哨兵。
    if ( !state ) return;

    ProcessingState* head = m_retiredStates.load(std::memory_order_relaxed);
    do {
        // 链头可被控制侧整批摘走，失败时 compare_exchange 更新 head 后重试。
        // 此处只链接地址，不解引用可能已被控制侧回收的旧链头。
        state->nextRetired = head;
    } while ( !m_retiredStates.compare_exchange_weak(
        head, state, std::memory_order_release, std::memory_order_relaxed) );
}

/// @brief 消费最新手动定位代际，将多个待处理请求合并为一次历史清理。
/// @warning 音频热路径，acquire 读取请求、release 发布消费进度，不等待控制侧。
/// @param resetStretcher 非旁路时是否同时清理后端，外层记账始终复位。
bool TimeStretcher::apply_discontinuity_request(bool resetStretcher)
{
    const std::uint64_t requested =
        m_requestedDiscontinuityGeneration.load(std::memory_order_acquire);
    const std::uint64_t consumed =
        m_consumedDiscontinuityGeneration.load(std::memory_order_relaxed);
    // 未见新请求就保留历史，频繁查询不能反复清空后端。
    if ( requested == consumed ) return false;

    // 先重置算法与记账，再确认代际，避免控制侧把尚未处理的请求当成已消费。
    reset_processing_history(resetStretcher);
    m_consumedDiscontinuityGeneration.store(requested,
                                            std::memory_order_release);
    return true;
}

/// @brief 读取外部 epoch，建立初始基线或在变化时清理当前历史。
/// @warning 音频热路径，最多尝试两次配置读取，不为等待控制写入而阻塞。
/// @param resetStretcher 显式边界已经重置后端时传 false，仅处理外层记账。
bool TimeStretcher::apply_provider_discontinuity(bool resetStretcher)
{
    // 配置变动与时间线代际是两类序号，不能用配置重写次数代替定位代际。
    for ( int attempt = 0; attempt < 2; ++attempt ) {
        const std::uint64_t sequenceBefore =
            m_providerConfigurationSequence.load(std::memory_order_acquire);
        // 奇数表示控制侧尚未发布完成，本次不读取可能混用的上下文和函数。
        if ( (sequenceBefore & 1U) != 0U ) return false;

        const DiscontinuityGenerationReader reader =
            m_discontinuityGenerationReader.load(std::memory_order_relaxed);
        const void* context =
            m_discontinuityProviderContext.load(std::memory_order_relaxed);
        const std::uint64_t sequenceAfter =
            m_providerConfigurationSequence.load(std::memory_order_acquire);
        // 序号变化时丢弃本次字段快照，有限次重试后交给下一次调用继续。
        if ( sequenceBefore != sequenceAfter ) continue;

        if ( !reader ) {
            // 清除 reader 后撤销本地基线，但不伪造一个新的外部代际。
            m_hasObservedProviderGeneration = false;
            return false;
        }

        // 配置序号不是生命周期保护，调用期间 context 必须仍然存活。
        const std::uint64_t generation = reader(context);
        // 第一次观察只建立基线，不把 provider 当前值当成刚发生的定位。
        if ( !m_hasObservedProviderGeneration ) {
            m_observedProviderGeneration    = generation;
            m_hasObservedProviderGeneration = true;
            m_publishedProviderGeneration.store(generation,
                                                std::memory_order_release);
            return false;
        }
        // 相同 epoch 是连续输入常态，不能逐段 reset 损失历史。
        if ( generation == m_observedProviderGeneration ) return false;

        // 同一 reader 后续代际变化才触发重置；即使不重置后端也会清外层记账。
        reset_processing_history(resetStretcher);
        m_observedProviderGeneration = generation;
        m_publishedProviderGeneration.store(generation,
                                            std::memory_order_release);
        return true;
    }
    return false;
}

/// @brief 清理连续流的分数余量、边界排空和结束记账，准备新流。
/// @warning 音频路径，只复用已预热状态；外部 provider 配置不在此销毁。
/// @param resetStretcher 为假时保留后端缓存，只清理应用层的连续流账目。
void TimeStretcher::reset_processing_history(bool resetStretcher)
{
    if ( m_currentState ) {
        // 旁路或显式边界后的确认阶段允许只重置外层状态，不再次清后端。
        if ( resetStretcher && m_currentState->stretcher ) {
            // 复位已预热实例，不在边界处理时调用新实例构造流程。
            m_currentState->stretcher->reset();
        }
        // 新流不能继承旧段的输入小数或输出预算，否则定位后时长会串段。
        m_currentState->finalSubmitted                       = false;
        m_currentState->finalDrained                         = false;
        m_currentState->inputFrameRemainder                  = 0.0;
        m_currentState->discontinuityDrainPending            = false;
        m_currentState->awaitingBoundaryEpochAcknowledgement = false;
        m_currentState->pendingInputFramesAfterBoundary      = 0U;
        m_currentState->exactStreamOutputFrames              = 0.0L;
        m_currentState->deliveredStreamOutputFrames          = 0U;
        m_currentState->terminalOutputFramesRemaining        = 0U;
    }
    m_finalInputDrained.store(false, std::memory_order_release);
    // 当前已经存在的 final 请求属于被清掉的历史，一并消费以免立即再次终止。
    m_consumedFinalGeneration.store(
        m_requestedFinalGeneration.load(std::memory_order_acquire),
        std::memory_order_release);
}

/// @brief 查询一个不超过剩余输入预算的连续区间，不直接拉取 PCM。
/// @warning 音频热路径最多检查两次配置，reader 必须不阻塞、不分配。
/// 读取配置失败时退回整段请求，并不等于已经证实区间内没有真实边界。
TimeStretcher::InputSpan TimeStretcher::read_input_span(
    std::size_t maxInputFrames) const
{
    // 无 reader 的兼容路径一次请求全部剩余帧，段尾没有额外动作。
    const InputSpan fallback{
        .frameCount = maxInputFrames,
        .boundary   = InputBoundary::None,
    };

    for ( int attempt = 0; attempt < 2; ++attempt ) {
        const std::uint64_t sequenceBefore =
            m_inputBoundaryConfigurationSequence.load(
                std::memory_order_acquire);
        // 不等待控制线程完成写入，维持音频回调有界执行。
        if ( (sequenceBefore & 1U) != 0U ) return fallback;

        const InputBoundaryReader reader =
            m_inputBoundaryReader.load(std::memory_order_relaxed);
        void* context =
            m_inputBoundaryProviderContext.load(std::memory_order_relaxed);
        const std::uint64_t sequenceAfter =
            m_inputBoundaryConfigurationSequence.load(
                std::memory_order_acquire);
        if ( sequenceBefore != sequenceAfter ) continue;
        if ( !reader ) return fallback;

        // reader 可维护自己的区间游标，查询时机必须与随后的上游拉取一致。
        const InputSpan span = reader(context, maxInputFrames);
        // 拒绝超额请求；零帧却带边界是有效结果，用于表达当前位置的动作。
        if ( span.frameCount > maxInputFrames ) return fallback;
        // 长度校验不证明边界真实，reader 仍须维持其查询游标语义。
        return span;
    }
    return fallback;
}

/// @brief 逐连续段复制上游 PCM，保留边界次序但不引入算法延迟。
/// @return 向上游请求的总输入帧数，不包含静音或旧段尾音前缀。
/// @warning 音频热路径使用预分配缓冲，不增加上游所有权或分配临时片段。
/// 旁路只接受精确单位倍率，输入预算与输出剩余窗口一一对应。
/// @param output 接收直接复制结果的设备块，未覆盖区域已由调用方清零。
/// @param inputNode 上游观察引用，处理期间不得由控制线程替换或销毁。
/// @param inputFrames 本次计划消费的源帧总数，不含先前已消费区间。
/// @param finalAtBlockEnd 入口已有 final 请求时，在计划输入末尾结束。
/// @param initialOutputOffset 输出中已经被旧段尾音占据的前缀长度。
std::size_t TimeStretcher::process_bypass_segments(
    AudioBuffer& output, IAudioNode& inputNode, std::size_t inputFrames,
    bool finalAtBlockEnd, std::size_t initialOutputOffset)
{
    // 缓冲由当前状态独占，上游只在拉取期间借用地址。
    AudioBuffer& inputBuffer  = m_currentState->inputBuffer;
    std::size_t  remaining    = inputFrames;
    std::size_t  outputOffset = initialOutputOffset;
    std::size_t  pulledFrames = 0U;

    // 即使输入预算为零也查询一次，以便消费当前位置的 Final 等边界动作。
    // 段数设硬上限，防止 reader 连续返回零长度边界导致音频线程无法退出。
    for ( std::size_t segmentIndex = 0U;
          segmentIndex < MAX_INPUT_SEGMENTS_PER_BLOCK &&
          (remaining > 0U || segmentIndex == 0U);
          ++segmentIndex ) {
        // 查询可能推进上游边界状态，先消费由查询引出的 epoch 或定位请求。
        const InputSpan span = read_input_span(remaining);
        apply_provider_discontinuity(false);
        apply_discontinuity_request(false);

        // 只激活该段需要的帧数，不因段很短就缩减下次可使用的存储容量。
        const std::size_t segmentFrames = span.frameCount;
        if ( !inputBuffer.set_active_frames(segmentFrames) ) {
            m_capacityOverflowCount.fetch_add(1U, std::memory_order_relaxed);
            break;
        }
        inputBuffer.clear();
        if ( segmentFrames > 0U ) {
            inputNode.process(inputBuffer);
            remaining -= segmentFrames;
            pulledFrames += segmentFrames;

            // 旁路没有算法尾音，按实际分段次序拼接到输出，不跨边界混合采样。
            // 旁路状态只允许精确单位倍率，因此累计输入不会超过输出剩余帧数。
            // 非单位倍率必须经过后端，不能靠丢弃输入或越界复制近似其时钟。
            float**             outputChannels = output.raw_ptrs();
            const float* const* inputChannels  = inputBuffer.raw_ptrs();
            for ( std::uint16_t channel = 0U; channel < output.num_channels();
                  ++channel ) {
                std::copy_n(inputChannels[channel],
                            segmentFrames,
                            outputChannels[channel] + outputOffset);
            }
            // 后续段只追加，不能覆盖先前已经交付的区间。
            outputOffset += segmentFrames;
        }

        // 上游 process 可能在这一段内发出 final，必须在继续请求下一段前观察。
        const bool newFinalRequest =
            !finalAtBlockEnd &&
            m_requestedFinalGeneration.load(std::memory_order_acquire) !=
                m_consumedFinalGeneration.load(std::memory_order_relaxed);
        // 显式 Final 终止当前段，旧邮箱终止计划末尾，新邮箱终止刚拉到的段。
        const bool submitFinal = span.boundary == InputBoundary::Final ||
                                 (finalAtBlockEnd && remaining == 0U) ||
                                 newFinalRequest;
        if ( submitFinal ) {
            // 旁路已直接交付全部 PCM，没有后端尾音需要继续 drain。
            m_currentState->finalSubmitted = true;
            m_currentState->finalDrained   = true;
            publish_final_state(
                m_requestedFinalGeneration.load(std::memory_order_acquire));
            break;
        }

        if ( span.boundary == InputBoundary::Discontinuity ) {
            // 无算法历史可混入新段，确认代际后允许在同一输出块继续复制。
            apply_provider_discontinuity(false);
            apply_discontinuity_request(false);
            continue;
        }
        apply_provider_discontinuity(false);
        apply_discontinuity_request(false);
        // 零帧且没有明确边界表示本次无法前进，不能无条件再次拉取同一区间。
        if ( segmentFrames == 0U ) break;
    }
    return pulledFrames;
}

/// @brief 将连续输入段送入变速器，在定位边界或 final 处限制并交付尾音。
/// @return 本次向上游请求的输入帧数；算法输出可能延迟到后续块。
/// @warning 音频热路径，只使用当前状态预备空间，不创建或销毁算法实例。
/// @param output 供所有分段复用的固定容量输出，写入游标只能向后推进。
/// @param inputNode 每段按明确长度拉取的上游，不在此增加共享所有权。
/// @param inputFrames 本次输入预算，未能在边界后消费的部分可跨块保留。
/// @param finalAtBlockEnd 区分入口已有的结束请求与本次上游拉取新发出的请求。
/// @param initialOutputOffset 本块旧段尾音长度，新输入的输出从此偏移继续。
std::size_t TimeStretcher::process_stretched_segments(
    AudioBuffer& output, IAudioNode& inputNode, std::size_t inputFrames,
    bool finalAtBlockEnd, std::size_t initialOutputOffset)
{
    AudioBuffer& inputBuffer  = m_currentState->inputBuffer;
    std::size_t  remaining    = inputFrames;
    std::size_t  outputOffset = initialOutputOffset;
    std::size_t  pulledFrames = 0U;

    // 一个设备块可跨多个短连续段，段数上限约束异常边界输入下的处理工作量。
    // 首次零输入迭代仍用于向后端发送结束信号，不能直接跳过整个循环。
    for ( std::size_t segmentIndex = 0U;
          segmentIndex < MAX_INPUT_SEGMENTS_PER_BLOCK &&
          (remaining > 0U || segmentIndex == 0U);
          ++segmentIndex ) {
        // 在上游拉取前查询边界，旧段 PCM 不能被当成新 epoch 的输入一起处理。
        const InputSpan span = read_input_span(remaining);
        const bool      suppressProviderReset =
            m_currentState->awaitingBoundaryEpochAcknowledgement;
        const bool providerChanged =
            apply_provider_discontinuity(!suppressProviderReset);
        if ( suppressProviderReset && providerChanged ) {
            m_currentState->awaitingBoundaryEpochAcknowledgement = false;
        }
        apply_discontinuity_request(true);

        const std::size_t segmentFrames = span.frameCount;
        if ( !inputBuffer.set_active_frames(segmentFrames) ) {
            m_capacityOverflowCount.fetch_add(1U, std::memory_order_relaxed);
            break;
        }
        // 上游无帧数返回值，清零保证它提前结束时未写部分仍有确定的静音值。
        inputBuffer.clear();
        if ( segmentFrames > 0U ) {
            inputNode.process(inputBuffer);
            remaining -= segmentFrames;
            pulledFrames += segmentFrames;
        }

        // 在拉取期间发出的 final 必须在当前段提交，而非额外多拉一个输入块。
        const bool newFinalRequest =
            !finalAtBlockEnd &&
            m_requestedFinalGeneration.load(std::memory_order_acquire) !=
                m_consumedFinalGeneration.load(std::memory_order_relaxed);
        const bool submitFinal = span.boundary == InputBoundary::Final ||
                                 (finalAtBlockEnd && remaining == 0U) ||
                                 newFinalRequest;
        // 段间 discontinuity 结束的是本次算法流，不结束后面的时间线输入。
        const bool closeForDiscontinuity =
            span.boundary == InputBoundary::Discontinuity;

        // 已显式给出旧段末端时，先排旧段尾音，不能在这里响应 epoch 清掉它。
        // 没有显式边界时则复查拉取期间发生的变化，防止旧历史污染刚定位的输入。
        if ( !closeForDiscontinuity ) {
            const bool suppressPostPullReset =
                m_currentState->awaitingBoundaryEpochAcknowledgement;
            const bool postPullProviderChanged =
                apply_provider_discontinuity(!suppressPostPullReset);
            if ( suppressPostPullReset && postPullProviderChanged ) {
                m_currentState->awaitingBoundaryEpochAcknowledgement = false;
            }
            apply_discontinuity_request(true);
        }

        // 累计理想输出时长而非逐段舍入，短段及小数倍率不会反复增加取整误差。
        m_currentState->exactStreamOutputFrames +=
            static_cast<long double>(segmentFrames) /
            static_cast<long double>(m_currentState->playbackRatio);

        // 两类封口共用算法 final，但只有 submitFinal 会阻止后续时间线输入。
        const bool terminalSegment = submitFinal || closeForDiscontinuity;
        if ( terminalSegment ) {
            // 结束段才锁定整数目标，扣除此前已经交付的帧数得到尾部预算。
            const std::size_t targetOutputFrames =
                roundedOutputFrames(m_currentState->exactStreamOutputFrames);
            // 已交付帧数可能达到目标，饱和到零避免无符号减法产生巨大预算。
            m_currentState->terminalOutputFramesRemaining =
                targetOutputFrames > m_currentState->deliveredStreamOutputFrames
                    ? targetOutputFrames -
                          m_currentState->deliveredStreamOutputFrames
                    : 0U;
        }

        // 连续段只受本块容量约束，终止段还必须裁掉超过应用目标时长的算法尾部。
        const std::size_t outputLimit =
            terminalSegment ? m_currentState->terminalOutputFramesRemaining
                            : output.num_frames() - outputOffset;
        // final 同时用于整流结束与段间封口，但后者排完还会重开一个新流。
        const std::size_t written =
            processWithOutputLimit(*m_currentState->stretcher,
                                   output,
                                   outputOffset,
                                   inputBuffer,
                                   terminalSegment,
                                   outputLimit);
        // 输入可能被后端全部消费而仅输出少量帧，两种记账不能互相代替。
        // outputOffset 只按实际输出推进，不能用已消费输入帧数替代。
        outputOffset += written;
        m_currentState->deliveredStreamOutputFrames += written;
        if ( terminalSegment ) {
            m_currentState->terminalOutputFramesRemaining -= written;
        }

        if ( submitFinal ) {
            // 尽量用本块空余位置交付尾音；没有立即可读帧时退出，不忙等后端。
            while ( outputOffset < output.num_frames() &&
                    m_currentState->terminalOutputFramesRemaining > 0U ) {
                const std::size_t drained = drainWithOutputLimit(
                    *m_currentState->stretcher,
                    output,
                    outputOffset,
                    m_currentState->terminalOutputFramesRemaining);
                outputOffset += drained;
                m_currentState->deliveredStreamOutputFrames += drained;
                m_currentState->terminalOutputFramesRemaining -= drained;
                // 无立即输出就退出，不通过 sleep 或忙等强迫后端产出。
                if ( drained == 0U ) break;
            }
            // 未交付完的预算由后续 process 继续排出，此后不再请求上游。
            publish_final_state(
                m_requestedFinalGeneration.load(std::memory_order_acquire));
            break;
        }

        if ( closeForDiscontinuity ) {
            // 旧段先占据输出窗口，不把其余输入立刻喂入即将被重置的同一后端。
            while ( outputOffset < output.num_frames() &&
                    m_currentState->terminalOutputFramesRemaining > 0U ) {
                const std::size_t drained = drainWithOutputLimit(
                    *m_currentState->stretcher,
                    output,
                    outputOffset,
                    m_currentState->terminalOutputFramesRemaining);
                outputOffset += drained;
                m_currentState->deliveredStreamOutputFrames += drained;
                m_currentState->terminalOutputFramesRemaining -= drained;
                if ( drained == 0U ) break;
            }

            // 预算交付完即可清旧历史，即使后端还有额外填充帧也不再输出。
            if ( m_currentState->terminalOutputFramesRemaining == 0U ) {
                reset_processing_history(true);
                // 下一次 epoch 变化可能只是刚处理边界的确认，避免二次 reset。
                m_currentState->awaitingBoundaryEpochAcknowledgement = true;
            } else {
                // 本块装不下尾音时保存尚未拉取的预算，下一块先排旧尾再继续新段。
                m_currentState->discontinuityDrainPending       = true;
                m_currentState->pendingInputFramesAfterBoundary = remaining;
                break;
            }
            continue;
        }
        // 没有边界、没有输入进展时停止本轮，输出未写区域保持入口清理的静音。
        if ( segmentFrames == 0U ) break;
    }
    return pulledFrames;
}

/// @brief 发布当前流的 final 消费代际与预算交付状态。
/// @warning 音频侧写入控制可读的 release 原子；不接管资源或执行阻塞回调。
/// @param finalGeneration 本次确认消费的邮箱代际，不是处理状态的参数代际。
void TimeStretcher::publish_final_state(std::uint64_t finalGeneration)
{
    // 已提交 final 与已交付完独立记账，非旁路可能跨多个设备块继续排尾音。
    m_currentState->finalSubmitted = true;
    m_currentState->finalDrained =
        m_currentState->bypass ||
        m_currentState->terminalOutputFramesRemaining == 0U;
    // 消费代际与排空标志分别发布，多次读取不构成原子快照。
    m_consumedFinalGeneration.store(finalGeneration, std::memory_order_release);
    m_finalInputDrained.store(m_currentState->finalDrained,
                              std::memory_order_release);
}

/// @brief 判断参数是否精确为单位速度、零音高，可直接一对一复制。
/// @warning 状态准备时选择路径，不是每样本运行的滤波或插值操作。
/// @param playbackRatio 已通过范围校验的期望输入/输出帧比。
/// @param pitchSemitones 已通过范围校验的半音偏移，旁路要求同时接近零。
bool TimeStretcher::should_bypass(double playbackRatio, double pitchSemitones)
{
    // 两个条件必须同时满足；只变音高或只变速度都仍需要后端处理。
    // 容差旁路会让输入小数余量累积成额外帧，破坏一对一复制的容量不变量。
    // 即使参数变化很小，也保留其时长和音高语义，交由预热后端处理。
    return playbackRatio == 1.0 && pitchSemitones == 0.0;
}

}  // namespace ice
