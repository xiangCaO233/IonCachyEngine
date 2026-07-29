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
constexpr std::size_t MAX_INPUT_SEGMENTS_PER_BLOCK = 4096U;

/// @brief 将连续输入对应的精确输出帧数转换为应用层目标帧数。
/// @param exactFrames 连续流累计的浮点输出帧数。
/// @return 四舍五入并限制在 size_t 范围内的帧数。
[[nodiscard]] std::size_t roundedOutputFrames(long double exactFrames)
{
    if ( exactFrames <= 0.0L ) return 0U;
    constexpr long double MAX_FRAMES =
        static_cast<long double>(std::numeric_limits<std::size_t>::max());
    if ( exactFrames >= MAX_FRAMES ) {
        return std::numeric_limits<std::size_t>::max();
    }
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
[[nodiscard]] std::size_t processWithOutputLimit(
    RStretcher& stretcher, AudioBuffer& output, std::size_t outputOffset,
    const AudioBuffer& input, bool finalInput, std::size_t maxWriteFrames)
{
    const std::size_t originalFrames = output.num_frames();
    const std::size_t writableFrames =
        std::min(maxWriteFrames, originalFrames - outputOffset);
    output.set_active_frames(outputOffset + writableFrames);
    const std::size_t written =
        stretcher.process_into(output, outputOffset, input, finalInput);
    output.set_active_frames(originalFrames);
    return written;
}

/// @brief 在不改变存储容量的前提下限制一次 RubberBand drain 长度。
/// @param stretcher 已提交 final 的 RubberBand 包装。
/// @param output 固定容量输出缓冲。
/// @param outputOffset 输出起点。
/// @param maxWriteFrames 本次允许写入的最大帧数。
/// @return 实际写入帧数。
[[nodiscard]] std::size_t drainWithOutputLimit(RStretcher&  stretcher,
                                               AudioBuffer& output,
                                               std::size_t  outputOffset,
                                               std::size_t  maxWriteFrames)
{
    const std::size_t originalFrames = output.num_frames();
    const std::size_t writableFrames =
        std::min(maxWriteFrames, originalFrames - outputOffset);
    output.set_active_frames(outputOffset + writableFrames);
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
        if ( !bypass ) {
            stretcher = std::make_unique<RStretcher>(format,
                                                     quality,
                                                     maxInputFrames,
                                                     maxOutputFrames,
                                                     initialStretchRatio,
                                                     initialPitchRatio);
        }
        inputBuffer.clear();
    }

    /// @brief 最大输出 block 帧数。
    std::size_t maxOutputFrames{ 0U };

    /// @brief 为最高播放速度预分配的最大输入帧数。
    std::size_t maxInputFrames{ 0U };

    /// @brief 复用的上游输入缓冲。
    AudioBuffer inputBuffer;

    /// @brief 此状态固定的播放速度倍率。
    double playbackRatio{ 1.0 };

    /// @brief 此状态固定的音高偏移。
    double pitchSemitones{ 0.0 };

    /// @brief 此状态是否采用直通路径。
    bool bypass{ true };

    /// @brief 非直通状态独占的预热 RubberBand 包装。
    std::unique_ptr<RStretcher> stretcher;

    /// @brief 跨 block 保留的输入帧小数余量。
    double inputFrameRemainder{ 0.0 };

    /// @brief 此状态是否已提交 final 输入。
    bool finalSubmitted{ false };

    /// @brief 此状态是否已经完全 drain。
    bool finalDrained{ false };

    /// @brief 是否正在跨 block drain discontinuity 前的旧段尾音。
    bool discontinuityDrainPending{ false };

    /// @brief 是否等待 epoch provider 确认已经处理的显式边界。
    bool awaitingBoundaryEpochAcknowledgement{ false };

    /// @brief discontinuity 跨 block drain 后仍需拉取的原 block 输入帧。
    std::size_t pendingInputFramesAfterBoundary{ 0U };

    /// @brief 当前连续算法流按输入倍率累计的精确目标输出帧。
    long double exactStreamOutputFrames{ 0.0L };

    /// @brief 当前连续算法流已经交付给下游的输出帧。
    std::size_t deliveredStreamOutputFrames{ 0U };

    /// @brief final 或 discontinuity 尚需交付的精确输出预算。
    std::size_t terminalOutputFramesRemaining{ 0U };

    /// @brief 状态代际。
    std::uint64_t generation{ 0U };

    /// @brief 无锁退役链的下一节点。
    ProcessingState* nextRetired{ nullptr };
};

TimeStretcher::TimeStretcher()
    : m_controlFormat(ICEConfig::internal_format)
    , m_controlMaxOutputFrames(
          std::max<std::size_t>(1U, ICEConfig::default_buffer_size))
{
    publish_prepared_state(m_controlFormat,
                           m_controlMaxOutputFrames,
                           m_quality.load(std::memory_order_relaxed));
    apply_pending_state();
}

TimeStretcher::~TimeStretcher()
{
    collect_retired_states();

    std::unique_ptr<ProcessingState> pending{ m_pendingState.exchange(
        nullptr, std::memory_order_acq_rel) };
    std::unique_ptr<ProcessingState> current{ m_currentState };
    m_currentState = nullptr;

    collect_retired_states();
}

void TimeStretcher::process(AudioBuffer& buffer)
{
    if ( m_paused.load(std::memory_order_acquire) ) {
        buffer.clear();
        return;
    }

    apply_pending_state();

    IAudioNode* inputNode = get_inputnode_observer();
    if ( !inputNode || !m_currentState ) {
        buffer.clear();
        return;
    }

    const bool resetStretcher = !m_currentState->bypass;
    if ( !m_currentState->discontinuityDrainPending ) {
        const bool suppressProviderReset =
            m_currentState->awaitingBoundaryEpochAcknowledgement;
        const bool providerChanged = apply_provider_discontinuity(
            resetStretcher && !suppressProviderReset);
        if ( suppressProviderReset && providerChanged ) {
            m_currentState->awaitingBoundaryEpochAcknowledgement = false;
        }
    }
    apply_discontinuity_request(resetStretcher);

    if ( m_currentState->finalSubmitted && m_currentState->finalDrained ) {
        buffer.clear();
        return;
    }

    if ( buffer.afmt != m_currentState->inputBuffer.afmt ||
         buffer.num_frames() > m_currentState->maxOutputFrames ) {
        m_capacityOverflowCount.fetch_add(1U, std::memory_order_relaxed);
        buffer.clear();
        return;
    }

    if ( m_currentState->finalSubmitted && !m_currentState->bypass ) {
        if ( !m_currentState->finalDrained ) {
            buffer.clear();
            const std::size_t written = drainWithOutputLimit(
                *m_currentState->stretcher,
                buffer,
                0U,
                m_currentState->terminalOutputFramesRemaining);
            m_currentState->deliveredStreamOutputFrames += written;
            m_currentState->terminalOutputFramesRemaining -= written;
            m_currentState->finalDrained =
                m_currentState->terminalOutputFramesRemaining == 0U;
            m_finalInputDrained.store(m_currentState->finalDrained,
                                      std::memory_order_release);
        } else {
            buffer.clear();
        }
        return;
    }

    const std::size_t outputFrames = buffer.num_frames();
    if ( outputFrames == 0U ) return;

    buffer.clear();
    std::size_t initialOutputOffset = 0U;
    if ( m_currentState->discontinuityDrainPending ) {
        initialOutputOffset =
            drainWithOutputLimit(*m_currentState->stretcher,
                                 buffer,
                                 0U,
                                 m_currentState->terminalOutputFramesRemaining);
        m_currentState->deliveredStreamOutputFrames += initialOutputOffset;
        m_currentState->terminalOutputFramesRemaining -= initialOutputOffset;
        if ( m_currentState->terminalOutputFramesRemaining > 0U ) {
            m_actualPlaybackRatio.store(0.0, std::memory_order_relaxed);
            return;
        }

        const std::size_t carriedInputFrames =
            m_currentState->pendingInputFramesAfterBoundary;
        reset_processing_history(true);
        m_currentState->pendingInputFramesAfterBoundary = carriedInputFrames;
        m_currentState->awaitingBoundaryEpochAcknowledgement = true;
        if ( initialOutputOffset >= outputFrames ) {
            m_actualPlaybackRatio.store(0.0, std::memory_order_relaxed);
            return;
        }
    }

    const std::size_t outputFramesToFill = outputFrames - initialOutputOffset;
    std::size_t       inputFrames        = 0U;
    if ( m_currentState->pendingInputFramesAfterBoundary > 0U ) {
        inputFrames = m_currentState->pendingInputFramesAfterBoundary;
        m_currentState->pendingInputFramesAfterBoundary = 0U;
    } else {
        const double requestedInputFrames =
            static_cast<double>(outputFramesToFill) *
                m_currentState->playbackRatio +
            m_currentState->inputFrameRemainder;
        inputFrames =
            static_cast<std::size_t>(std::floor(requestedInputFrames));
        m_currentState->inputFrameRemainder =
            requestedInputFrames - static_cast<double>(inputFrames);
    }
    if ( inputFrames > m_currentState->maxInputFrames ) {
        m_capacityOverflowCount.fetch_add(1U, std::memory_order_relaxed);
        buffer.clear();
        return;
    }

    const std::uint64_t requestedFinal =
        m_requestedFinalGeneration.load(std::memory_order_acquire);
    const std::uint64_t consumedFinal =
        m_consumedFinalGeneration.load(std::memory_order_relaxed);
    const bool finalAtBlockEnd = requestedFinal != consumedFinal;

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
    const double actualRatio = static_cast<double>(pulledInputFrames) /
                               static_cast<double>(outputFramesToFill);
    m_actualPlaybackRatio.store(actualRatio, std::memory_order_relaxed);
}

bool TimeStretcher::prepare(const AudioDataFormat& format,
                            std::size_t            maxOutputFrames)
{
    constexpr std::size_t MAX_RATIO_FRAMES =
        static_cast<std::size_t>(MAX_PLAYBACK_RATIO);
    if ( format.channels == 0U || format.samplerate == 0U ||
         maxOutputFrames == 0U ||
         maxOutputFrames > (std::numeric_limits<std::size_t>::max() - 1U) /
                               MAX_RATIO_FRAMES ) {
        return false;
    }

    m_controlFormat          = format;
    m_controlMaxOutputFrames = maxOutputFrames;
    return publish_prepared_state(
        format, maxOutputFrames, m_quality.load(std::memory_order_acquire));
}

void TimeStretcher::set_playback_ratio(double desiredRatio)
{
    if ( !std::isfinite(desiredRatio) || desiredRatio < MIN_PLAYBACK_RATIO ||
         desiredRatio > MAX_PLAYBACK_RATIO ) {
        return;
    }

    const double previous = m_desiredPlaybackRatio.exchange(
        desiredRatio, std::memory_order_acq_rel);
    if ( std::abs(previous - desiredRatio) <=
         std::numeric_limits<double>::epsilon() ) {
        return;
    }
    publish_prepared_state(m_controlFormat,
                           m_controlMaxOutputFrames,
                           m_quality.load(std::memory_order_acquire));
}

void TimeStretcher::set_pitch_semitones(double semitones)
{
    if ( !std::isfinite(semitones) || semitones < -24.0 || semitones > 24.0 ) {
        return;
    }

    const double previous =
        m_desiredPitchSemitones.exchange(semitones, std::memory_order_acq_rel);
    if ( std::abs(previous - semitones) <=
         std::numeric_limits<double>::epsilon() ) {
        return;
    }
    publish_prepared_state(m_controlFormat,
                           m_controlMaxOutputFrames,
                           m_quality.load(std::memory_order_acquire));
}

double TimeStretcher::get_actual_playback_ratio() const
{
    return m_actualPlaybackRatio.load(std::memory_order_relaxed);
}

double TimeStretcher::get_pitch_semitones() const
{
    return m_desiredPitchSemitones.load(std::memory_order_relaxed);
}

void TimeStretcher::set_quality(TimeStretchQuality quality)
{
    const TimeStretchQuality previous =
        m_quality.exchange(quality, std::memory_order_acq_rel);
    if ( previous == quality ) return;

    publish_prepared_state(m_controlFormat, m_controlMaxOutputFrames, quality);
}

TimeStretchQuality TimeStretcher::get_quality() const
{
    return m_quality.load(std::memory_order_acquire);
}

void TimeStretcher::set_paused(bool paused) noexcept
{
    m_paused.store(paused, std::memory_order_release);
}

bool TimeStretcher::is_paused() const noexcept
{
    return m_paused.load(std::memory_order_acquire);
}

std::uint64_t TimeStretcher::request_discontinuity() noexcept
{
    return m_requestedDiscontinuityGeneration.fetch_add(
               1U, std::memory_order_release) +
           1U;
}

std::uint64_t TimeStretcher::consumed_discontinuity_generation() const
{
    return m_consumedDiscontinuityGeneration.load(std::memory_order_acquire);
}

void TimeStretcher::set_discontinuity_generation_provider(
    const void* context, DiscontinuityGenerationReader reader)
{
    m_providerConfigurationSequence.fetch_add(1U, std::memory_order_acq_rel);
    m_discontinuityProviderContext.store(context, std::memory_order_relaxed);
    m_discontinuityGenerationReader.store(reader, std::memory_order_relaxed);
    m_providerConfigurationSequence.fetch_add(1U, std::memory_order_release);
}

void TimeStretcher::clear_discontinuity_generation_provider()
{
    set_discontinuity_generation_provider(nullptr, nullptr);
}

std::uint64_t TimeStretcher::observed_provider_discontinuity_generation() const
{
    return m_publishedProviderGeneration.load(std::memory_order_acquire);
}

void TimeStretcher::set_input_boundary_provider(void*               context,
                                                InputBoundaryReader reader)
{
    m_inputBoundaryConfigurationSequence.fetch_add(1U,
                                                   std::memory_order_acq_rel);
    m_inputBoundaryProviderContext.store(context, std::memory_order_relaxed);
    m_inputBoundaryReader.store(reader, std::memory_order_relaxed);
    m_inputBoundaryConfigurationSequence.fetch_add(1U,
                                                   std::memory_order_release);
}

void TimeStretcher::clear_input_boundary_provider()
{
    set_input_boundary_provider(nullptr, nullptr);
}

std::uint64_t TimeStretcher::request_final_input() noexcept
{
    m_finalInputDrained.store(false, std::memory_order_release);
    return m_requestedFinalGeneration.fetch_add(1U, std::memory_order_release) +
           1U;
}

std::uint64_t TimeStretcher::consumed_final_generation() const
{
    return m_consumedFinalGeneration.load(std::memory_order_acquire);
}

bool TimeStretcher::is_final_input_drained() const
{
    return m_finalInputDrained.load(std::memory_order_acquire);
}

void TimeStretcher::collect_retired_states()
{
    ProcessingState* retired =
        m_retiredStates.exchange(nullptr, std::memory_order_acq_rel);
    while ( retired ) {
        ProcessingState*                 next = retired->nextRetired;
        std::unique_ptr<ProcessingState> reclaim{ retired };
        retired = next;
    }
}

std::uint64_t TimeStretcher::active_state_generation() const
{
    return m_activeStateGeneration.load(std::memory_order_acquire);
}

std::uint64_t TimeStretcher::capacity_overflow_count() const
{
    return m_capacityOverflowCount.load(std::memory_order_relaxed);
}

void TimeStretcher::apply_effect(AudioBuffer& output, const AudioBuffer& input)
{
    if ( !m_currentState || !m_currentState->stretcher ) {
        output.clear();
        return;
    }
    m_currentState->stretcher->process(output, input, false);
}

bool TimeStretcher::publish_prepared_state(const AudioDataFormat& format,
                                           std::size_t        maxOutputFrames,
                                           TimeStretchQuality quality)
{
    collect_retired_states();

    const double playbackRatio =
        m_desiredPlaybackRatio.load(std::memory_order_acquire);
    const double pitchSemitones =
        m_desiredPitchSemitones.load(std::memory_order_acquire);
    const double initialStretchRatio = 1.0 / playbackRatio;
    const double initialPitchRatio   = std::pow(2.0, pitchSemitones / 12.0);
    const std::uint64_t generation   = m_nextStateGeneration++;

    auto prepared = std::make_unique<ProcessingState>(format,
                                                      quality,
                                                      maxOutputFrames,
                                                      initialStretchRatio,
                                                      initialPitchRatio,
                                                      playbackRatio,
                                                      pitchSemitones,
                                                      generation);
    ProcessingState*                 published = prepared.release();
    std::unique_ptr<ProcessingState> superseded{ m_pendingState.exchange(
        published, std::memory_order_acq_rel) };
    return true;
}

void TimeStretcher::apply_pending_state()
{
    if ( m_currentState &&
         ((m_currentState->finalSubmitted && !m_currentState->finalDrained) ||
          m_currentState->discontinuityDrainPending) ) {
        return;
    }

    ProcessingState* pending =
        m_pendingState.exchange(nullptr, std::memory_order_acq_rel);
    if ( !pending ) return;

    const bool preserveTerminalState = m_currentState &&
                                       m_currentState->finalSubmitted &&
                                       m_currentState->finalDrained;
    pending->finalSubmitted          = preserveTerminalState;
    pending->finalDrained            = preserveTerminalState;

    ProcessingState* previous = m_currentState;
    m_currentState            = pending;
    m_activeStateGeneration.store(pending->generation,
                                  std::memory_order_release);
    m_actualPlaybackRatio.store(pending->playbackRatio,
                                std::memory_order_relaxed);
    m_finalInputDrained.store(preserveTerminalState, std::memory_order_release);
    retire_state(previous);
}

void TimeStretcher::retire_state(ProcessingState* state)
{
    if ( !state ) return;

    ProcessingState* head = m_retiredStates.load(std::memory_order_relaxed);
    do {
        state->nextRetired = head;
    } while ( !m_retiredStates.compare_exchange_weak(
        head, state, std::memory_order_release, std::memory_order_relaxed) );
}

bool TimeStretcher::apply_discontinuity_request(bool resetStretcher)
{
    const std::uint64_t requested =
        m_requestedDiscontinuityGeneration.load(std::memory_order_acquire);
    const std::uint64_t consumed =
        m_consumedDiscontinuityGeneration.load(std::memory_order_relaxed);
    if ( requested == consumed ) return false;

    reset_processing_history(resetStretcher);
    m_consumedDiscontinuityGeneration.store(requested,
                                            std::memory_order_release);
    return true;
}

bool TimeStretcher::apply_provider_discontinuity(bool resetStretcher)
{
    for ( int attempt = 0; attempt < 2; ++attempt ) {
        const std::uint64_t sequenceBefore =
            m_providerConfigurationSequence.load(std::memory_order_acquire);
        if ( (sequenceBefore & 1U) != 0U ) return false;

        const DiscontinuityGenerationReader reader =
            m_discontinuityGenerationReader.load(std::memory_order_relaxed);
        const void* context =
            m_discontinuityProviderContext.load(std::memory_order_relaxed);
        const std::uint64_t sequenceAfter =
            m_providerConfigurationSequence.load(std::memory_order_acquire);
        if ( sequenceBefore != sequenceAfter ) continue;

        if ( !reader ) {
            m_hasObservedProviderGeneration = false;
            return false;
        }

        const std::uint64_t generation = reader(context);
        if ( !m_hasObservedProviderGeneration ) {
            m_observedProviderGeneration    = generation;
            m_hasObservedProviderGeneration = true;
            m_publishedProviderGeneration.store(generation,
                                                std::memory_order_release);
            return false;
        }
        if ( generation == m_observedProviderGeneration ) return false;

        reset_processing_history(resetStretcher);
        m_observedProviderGeneration = generation;
        m_publishedProviderGeneration.store(generation,
                                            std::memory_order_release);
        return true;
    }
    return false;
}

void TimeStretcher::reset_processing_history(bool resetStretcher)
{
    if ( m_currentState ) {
        if ( resetStretcher && m_currentState->stretcher ) {
            m_currentState->stretcher->reset();
        }
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
    m_consumedFinalGeneration.store(
        m_requestedFinalGeneration.load(std::memory_order_acquire),
        std::memory_order_release);
}

TimeStretcher::InputSpan TimeStretcher::read_input_span(
    std::size_t maxInputFrames) const
{
    const InputSpan fallback{
        .frameCount = maxInputFrames,
        .boundary   = InputBoundary::None,
    };

    for ( int attempt = 0; attempt < 2; ++attempt ) {
        const std::uint64_t sequenceBefore =
            m_inputBoundaryConfigurationSequence.load(
                std::memory_order_acquire);
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

        const InputSpan span = reader(context, maxInputFrames);
        if ( span.frameCount > maxInputFrames ) return fallback;
        return span;
    }
    return fallback;
}

std::size_t TimeStretcher::process_bypass_segments(
    AudioBuffer& output, IAudioNode& inputNode, std::size_t inputFrames,
    bool finalAtBlockEnd, std::size_t initialOutputOffset)
{
    AudioBuffer& inputBuffer  = m_currentState->inputBuffer;
    std::size_t  remaining    = inputFrames;
    std::size_t  outputOffset = initialOutputOffset;
    std::size_t  pulledFrames = 0U;

    for ( std::size_t segmentIndex = 0U;
          segmentIndex < MAX_INPUT_SEGMENTS_PER_BLOCK &&
          (remaining > 0U || segmentIndex == 0U);
          ++segmentIndex ) {
        const InputSpan span = read_input_span(remaining);
        apply_provider_discontinuity(false);
        apply_discontinuity_request(false);

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

            float**             outputChannels = output.raw_ptrs();
            const float* const* inputChannels  = inputBuffer.raw_ptrs();
            for ( std::uint16_t channel = 0U; channel < output.num_channels();
                  ++channel ) {
                std::copy_n(inputChannels[channel],
                            segmentFrames,
                            outputChannels[channel] + outputOffset);
            }
            outputOffset += segmentFrames;
        }

        const bool newFinalRequest =
            !finalAtBlockEnd &&
            m_requestedFinalGeneration.load(std::memory_order_acquire) !=
                m_consumedFinalGeneration.load(std::memory_order_relaxed);
        const bool submitFinal = span.boundary == InputBoundary::Final ||
                                 (finalAtBlockEnd && remaining == 0U) ||
                                 newFinalRequest;
        if ( submitFinal ) {
            m_currentState->finalSubmitted = true;
            m_currentState->finalDrained   = true;
            publish_final_state(
                m_requestedFinalGeneration.load(std::memory_order_acquire));
            break;
        }

        if ( span.boundary == InputBoundary::Discontinuity ) {
            apply_provider_discontinuity(false);
            apply_discontinuity_request(false);
            continue;
        }
        apply_provider_discontinuity(false);
        apply_discontinuity_request(false);
        if ( segmentFrames == 0U ) break;
    }
    return pulledFrames;
}

std::size_t TimeStretcher::process_stretched_segments(
    AudioBuffer& output, IAudioNode& inputNode, std::size_t inputFrames,
    bool finalAtBlockEnd, std::size_t initialOutputOffset)
{
    AudioBuffer& inputBuffer  = m_currentState->inputBuffer;
    std::size_t  remaining    = inputFrames;
    std::size_t  outputOffset = initialOutputOffset;
    std::size_t  pulledFrames = 0U;

    for ( std::size_t segmentIndex = 0U;
          segmentIndex < MAX_INPUT_SEGMENTS_PER_BLOCK &&
          (remaining > 0U || segmentIndex == 0U);
          ++segmentIndex ) {
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
        inputBuffer.clear();
        if ( segmentFrames > 0U ) {
            inputNode.process(inputBuffer);
            remaining -= segmentFrames;
            pulledFrames += segmentFrames;
        }

        const bool newFinalRequest =
            !finalAtBlockEnd &&
            m_requestedFinalGeneration.load(std::memory_order_acquire) !=
                m_consumedFinalGeneration.load(std::memory_order_relaxed);
        const bool submitFinal = span.boundary == InputBoundary::Final ||
                                 (finalAtBlockEnd && remaining == 0U) ||
                                 newFinalRequest;
        const bool closeForDiscontinuity =
            span.boundary == InputBoundary::Discontinuity;

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

        m_currentState->exactStreamOutputFrames +=
            static_cast<long double>(segmentFrames) /
            static_cast<long double>(m_currentState->playbackRatio);

        const bool terminalSegment = submitFinal || closeForDiscontinuity;
        if ( terminalSegment ) {
            const std::size_t targetOutputFrames =
                roundedOutputFrames(m_currentState->exactStreamOutputFrames);
            m_currentState->terminalOutputFramesRemaining =
                targetOutputFrames > m_currentState->deliveredStreamOutputFrames
                    ? targetOutputFrames -
                          m_currentState->deliveredStreamOutputFrames
                    : 0U;
        }

        const std::size_t outputLimit =
            terminalSegment ? m_currentState->terminalOutputFramesRemaining
                            : output.num_frames() - outputOffset;
        const std::size_t written =
            processWithOutputLimit(*m_currentState->stretcher,
                                   output,
                                   outputOffset,
                                   inputBuffer,
                                   terminalSegment,
                                   outputLimit);
        outputOffset += written;
        m_currentState->deliveredStreamOutputFrames += written;
        if ( terminalSegment ) {
            m_currentState->terminalOutputFramesRemaining -= written;
        }

        if ( submitFinal ) {
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
            publish_final_state(
                m_requestedFinalGeneration.load(std::memory_order_acquire));
            break;
        }

        if ( closeForDiscontinuity ) {
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

            if ( m_currentState->terminalOutputFramesRemaining == 0U ) {
                reset_processing_history(true);
                m_currentState->awaitingBoundaryEpochAcknowledgement = true;
            } else {
                m_currentState->discontinuityDrainPending       = true;
                m_currentState->pendingInputFramesAfterBoundary = remaining;
                break;
            }
            continue;
        }
        if ( segmentFrames == 0U ) break;
    }
    return pulledFrames;
}

void TimeStretcher::publish_final_state(std::uint64_t finalGeneration)
{
    m_currentState->finalSubmitted = true;
    m_currentState->finalDrained =
        m_currentState->bypass ||
        m_currentState->terminalOutputFramesRemaining == 0U;
    m_consumedFinalGeneration.store(finalGeneration, std::memory_order_release);
    m_finalInputDrained.store(m_currentState->finalDrained,
                              std::memory_order_release);
}

bool TimeStretcher::should_bypass(double playbackRatio, double pitchSemitones)
{
    return std::abs(playbackRatio - 1.0) < 0.001 &&
           std::abs(pitchSemitones) < 0.001;
}

}  // namespace ice
