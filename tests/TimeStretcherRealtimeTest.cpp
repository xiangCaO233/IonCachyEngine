#include "ice/core/IAudioNode.hpp"
#include "ice/core/effect/TimeStretcher.hpp"
#include "ice/manage/AudioBuffer.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory>
#include <new>
#include <string_view>
#include <thread>

namespace
{

/// @brief 当前线程是否正在统计普通堆分配。
thread_local bool g_trackAllocations{ false };

/// @brief 当前统计区间内的普通堆分配数量。
thread_local std::size_t g_allocationCount{ 0U };

/// @brief 当前统计区间内的普通堆释放数量。
thread_local std::size_t g_deallocationCount{ 0U };

/// @brief 当前统计区间内的 C 堆分配调用数量。
thread_local std::size_t g_mallocCount{ 0U };

/// @brief 当前统计区间内的 C 堆释放调用数量。
thread_local std::size_t g_freeCount{ 0U };

/// @brief 测试时间线提供的 discontinuity 代际。
struct TestTimelineEpoch {
    /// @brief 当前时间线 epoch。
    std::atomic<std::uint64_t> generation{ 0U };
};

/// @brief 无分配的输入连续区间脚本。
struct BoundaryScript {
    /// @brief 单个预设连续区间。
    struct Step {
        /// @brief 区间输入帧数。
        std::size_t frameCount{ 0U };

        /// @brief 区间末端动作。
        ice::TimeStretcher::InputBoundary boundary{
            ice::TimeStretcher::InputBoundary::None
        };
    };

    /// @brief 添加一个测试区间。
    /// @param frameCount 区间帧数。
    /// @param boundary 区间末端动作。
    void add(std::size_t frameCount, ice::TimeStretcher::InputBoundary boundary)
    {
        if ( m_stepCount >= m_steps.size() ) return;
        m_steps[m_stepCount++] = {
            .frameCount = frameCount,
            .boundary   = boundary,
        };
    }

    /// @brief 返回下一段不超过 maxInputFrames 的连续区间。
    /// @param maxInputFrames 当前拉取上限。
    /// @return 连续区间及段尾动作。
    ice::TimeStretcher::InputSpan read(std::size_t maxInputFrames) noexcept
    {
        ++m_callbackCount;
        if ( m_stepIndex >= m_stepCount ) {
            return {
                .frameCount = maxInputFrames,
                .boundary   = ice::TimeStretcher::InputBoundary::None,
            };
        }

        Step& step = m_steps[m_stepIndex];
        if ( step.frameCount > maxInputFrames ) {
            step.frameCount -= maxInputFrames;
            return {
                .frameCount = maxInputFrames,
                .boundary   = ice::TimeStretcher::InputBoundary::None,
            };
        }

        const ice::TimeStretcher::InputSpan result{
            .frameCount = step.frameCount,
            .boundary   = step.boundary,
        };
        ++m_stepIndex;
        return result;
    }

    /// @brief 获取 provider 被查询的次数。
    /// @return 查询次数。
    [[nodiscard]] std::size_t callbackCount() const { return m_callbackCount; }

private:
    /// @brief 固定容量脚本。
    std::array<Step, 8U> m_steps{};

    /// @brief 有效脚本步数。
    std::size_t m_stepCount{ 0U };

    /// @brief 下一脚本索引。
    std::size_t m_stepIndex{ 0U };

    /// @brief provider 查询次数。
    std::size_t m_callbackCount{ 0U };
};

/// @brief 调用测试输入边界 provider。
/// @param context BoundaryScript。
/// @param maxInputFrames 当前拉取上限。
/// @return 连续区间查询结果。
ice::TimeStretcher::InputSpan readInputBoundary(
    void* context, std::size_t maxInputFrames) noexcept
{
    if ( !context ) {
        return {
            .frameCount = maxInputFrames,
            .boundary   = ice::TimeStretcher::InputBoundary::None,
        };
    }
    return static_cast<BoundaryScript*>(context)->read(maxInputFrames);
}

/// @brief 从测试时间线上读取 epoch。
/// @param context 指向 TestTimelineEpoch。
/// @return 当前 epoch。
std::uint64_t readTimelineEpoch(const void* context) noexcept
{
    if ( !context ) return 0U;
    const auto* epoch = static_cast<const TestTimelineEpoch*>(context);
    return epoch->generation.load(std::memory_order_acquire);
}

/// @brief 生成固定幅度锯齿波的无分配测试节点。
class SignalNode final : public ice::IAudioNode
{
public:
    /// @brief 写入一块测试音频。
    /// @param buffer 输出缓冲。
    /// @warning 测试音频热路径：不得分配内存。
    void process(ice::AudioBuffer& buffer) override
    {
        m_callCount.fetch_add(1U, std::memory_order_relaxed);
        m_processedFrames.fetch_add(buffer.num_frames(),
                                    std::memory_order_relaxed);
        if ( m_epochToAdvance ) {
            m_epochToAdvance->generation.store(m_generationToPublish,
                                               std::memory_order_release);
            m_epochToAdvance = nullptr;
        }
        const float amplitude = m_amplitude.load(std::memory_order_relaxed);
        float**     samples   = buffer.raw_ptrs();
        if ( !samples ) return;

        for ( std::uint16_t channel = 0U; channel < buffer.num_channels();
              ++channel ) {
            for ( std::size_t frame = 0U; frame < buffer.num_frames();
                  ++frame ) {
                const float phase =
                    static_cast<float>((m_frame + frame) % 32U) / 31.0F;
                samples[channel][frame] = amplitude * (phase * 2.0F - 1.0F);
            }
        }
        m_frame += buffer.num_frames();
    }

    /// @brief 设置后续输出幅度。
    /// @param amplitude 新幅度。
    void setAmplitude(float amplitude)
    {
        m_amplitude.store(amplitude, std::memory_order_relaxed);
    }

    /// @brief 获取被拉取次数。
    /// @return process 调用次数。
    [[nodiscard]] std::uint64_t callCount() const
    {
        return m_callCount.load(std::memory_order_relaxed);
    }

    /// @brief 获取上游累计提供的帧数。
    /// @return 所有 process 请求的逻辑帧数之和。
    [[nodiscard]] std::uint64_t processedFrames() const
    {
        return m_processedFrames.load(std::memory_order_relaxed);
    }

    /// @brief 让下一次上游拉取内部发布新的时间线 epoch。
    /// @param epoch 常驻测试时间线。
    /// @param generation 待发布代际。
    void advanceEpochDuringNextProcess(TestTimelineEpoch& epoch,
                                       std::uint64_t      generation)
    {
        m_epochToAdvance      = &epoch;
        m_generationToPublish = generation;
    }

private:
    /// @brief 当前输出幅度。
    std::atomic<float> m_amplitude{ 0.5F };

    /// @brief 连续波形帧位置。
    std::size_t m_frame{ 0U };

    /// @brief 被上层拉取的次数。
    std::atomic<std::uint64_t> m_callCount{ 0U };

    /// @brief 被上层请求的累计帧数。
    std::atomic<std::uint64_t> m_processedFrames{ 0U };

    /// @brief 下一次 process 内需要更新的测试时间线。
    TestTimelineEpoch* m_epochToAdvance{ nullptr };

    /// @brief 下一次 process 内发布的时间线代际。
    std::uint64_t m_generationToPublish{ 0U };
};

/// @brief 按拉取次数输出分段常量并记录每次请求大小。
class SegmentSignalNode final : public ice::IAudioNode
{
public:
    /// @brief 写入当前段常量值。
    /// @param buffer 输出缓冲。
    /// @warning 测试音频热路径：只写固定数组，不分配内存。
    void process(ice::AudioBuffer& buffer) override
    {
        const std::size_t callIndex = m_callCount;
        if ( callIndex < m_requestedFrames.size() ) {
            m_requestedFrames[callIndex] = buffer.num_frames();
        }
        ++m_callCount;
        m_processedFrames += buffer.num_frames();

        const float value   = callIndex == 0U ? m_firstValue : m_followingValue;
        float**     samples = buffer.raw_ptrs();
        for ( std::uint16_t channel = 0U; channel < buffer.num_channels();
              ++channel ) {
            std::fill_n(samples[channel], buffer.num_frames(), value);
        }

        if ( callIndex == 0U && m_epoch ) {
            m_epoch->generation.store(m_epochAfterFirstPull,
                                      std::memory_order_release);
        }
    }

    /// @brief 设置第一段与后续段的常量值。
    /// @param firstValue 第一段值。
    /// @param followingValue 后续段值。
    void setValues(float firstValue, float followingValue)
    {
        m_firstValue     = firstValue;
        m_followingValue = followingValue;
    }

    /// @brief 在第一次拉取结束时发布新 epoch。
    /// @param epoch 常驻测试 epoch。
    /// @param generation 新代际。
    void publishEpochAfterFirstPull(TestTimelineEpoch& epoch,
                                    std::uint64_t      generation)
    {
        m_epoch               = &epoch;
        m_epochAfterFirstPull = generation;
    }

    /// @brief 获取拉取次数。
    /// @return process 调用次数。
    [[nodiscard]] std::size_t callCount() const { return m_callCount; }

    /// @brief 获取指定拉取的帧数。
    /// @param index 拉取索引。
    /// @return 对应帧数，越界返回零。
    [[nodiscard]] std::size_t requestedFrames(std::size_t index) const
    {
        return index < m_callCount && index < m_requestedFrames.size()
                   ? m_requestedFrames[index]
                   : 0U;
    }

    /// @brief 获取累计拉取帧数。
    /// @return 所有请求帧数之和。
    [[nodiscard]] std::size_t processedFrames() const
    {
        return m_processedFrames;
    }

private:
    /// @brief 每次拉取大小。
    std::array<std::size_t, 64U> m_requestedFrames{};

    /// @brief 拉取次数。
    std::size_t m_callCount{ 0U };

    /// @brief 累计拉取帧数。
    std::size_t m_processedFrames{ 0U };

    /// @brief 第一段输出值。
    float m_firstValue{ 0.6F };

    /// @brief 后续段输出值。
    float m_followingValue{ -0.6F };

    /// @brief 第一次拉取后更新的 epoch。
    TestTimelineEpoch* m_epoch{ nullptr };

    /// @brief 第一次拉取后发布的 epoch。
    std::uint64_t m_epochAfterFirstPull{ 0U };
};

/// @brief 检查测试条件。
/// @param condition 必须成立的条件。
/// @param label 失败标签。
/// @return 成立时返回 0，否则返回 1。
int expectTrue(bool condition, std::string_view label)
{
    if ( condition ) return 0;
    std::fprintf(stderr,
                 "TimeStretcher realtime assertion failed: %.*s\n",
                 static_cast<int>(label.size()),
                 label.data());
    return 1;
}

/// @brief 检查缓冲是否为静音。
/// @param buffer 待检查缓冲。
/// @return 全部采样接近零时返回 true。
bool isSilent(const ice::AudioBuffer& buffer)
{
    const float* const* samples = buffer.raw_ptrs();
    if ( !samples ) return true;
    for ( std::uint16_t channel = 0U; channel < buffer.num_channels();
          ++channel ) {
        for ( std::size_t frame = 0U; frame < buffer.num_frames(); ++frame ) {
            if ( std::abs(samples[channel][frame]) > 0.000001F ) return false;
        }
    }
    return true;
}

/// @brief 将首声道采样合并到跨 block 极值。
/// @param buffer 待观察缓冲。
/// @param minimum 当前最小值。
/// @param maximum 当前最大值。
void observeExtrema(const ice::AudioBuffer& buffer, float& minimum,
                    float& maximum)
{
    const float* const* samples = buffer.raw_ptrs();
    if ( !samples || buffer.num_channels() == 0U ) return;
    for ( std::size_t frame = 0U; frame < buffer.num_frames(); ++frame ) {
        minimum = std::min(minimum, samples[0][frame]);
        maximum = std::max(maximum, samples[0][frame]);
    }
}

/// @brief 统计首声道中可辨识的非静音帧。
/// @param buffer 待检查缓冲。
/// @return 绝对值超过测试阈值的帧数。
std::size_t countAudibleFrames(const ice::AudioBuffer& buffer)
{
    const float* const* samples = buffer.raw_ptrs();
    if ( !samples || buffer.num_channels() == 0U ) return 0U;

    std::size_t audibleFrames = 0U;
    for ( std::size_t frame = 0U; frame < buffer.num_frames(); ++frame ) {
        if ( std::abs(samples[0][frame]) > 0.000001F ) ++audibleFrames;
    }
    return audibleFrames;
}

/// @brief 清零计数器并开始统计当前线程堆操作。
void beginHeapTracking()
{
    g_allocationCount   = 0U;
    g_deallocationCount = 0U;
    g_mallocCount       = 0U;
    g_freeCount         = 0U;
    g_trackAllocations  = true;
}

/// @brief 停止统计当前线程堆操作。
void endHeapTracking()
{
    g_trackAllocations = false;
}

/// @brief 验证统计窗口没有任何 C++ 或 C 堆分配。
/// @param label 失败标签。
/// @return 失败断言数量。
int expectNoHeapAllocation(std::string_view label)
{
    if ( g_allocationCount == 0U && g_mallocCount == 0U ) return 0;
    std::fprintf(stderr,
                 "Heap allocation counters: new=%zu malloc=%zu\n",
                 g_allocationCount,
                 g_mallocCount);
    return expectTrue(false, label);
}

/// @brief 验证统计窗口没有任何 C++ 或 C 堆释放。
/// @param label 失败标签。
/// @return 失败断言数量。
int expectNoHeapDeallocation(std::string_view label)
{
    if ( g_deallocationCount == 0U && g_freeCount == 0U ) return 0;
    std::fprintf(stderr,
                 "Heap deallocation counters: delete=%zu free=%zu\n",
                 g_deallocationCount,
                 g_freeCount);
    return expectTrue(false, label);
}

/// @brief 验证预热后切速、状态切换与 reset 的回调零分配。
/// @return 失败断言数量。
int testRealtimeStateAndReset()
{
    constexpr ice::AudioDataFormat FORMAT{
        .channels   = 2U,
        .samplerate = 48000U,
    };
    constexpr std::size_t BLOCK_FRAMES = 256U;

    auto               source = std::make_shared<SignalNode>();
    ice::TimeStretcher stretcher;
    stretcher.set_inputnode(source);
    stretcher.set_playback_ratio(0.75);
    stretcher.set_pitch_semitones(2.0);
    TestTimelineEpoch timelineEpoch;
    stretcher.set_discontinuity_generation_provider(&timelineEpoch,
                                                    &readTimelineEpoch);
    const bool       prepared = stretcher.prepare(FORMAT, BLOCK_FRAMES);
    ice::AudioBuffer output(FORMAT, BLOCK_FRAMES);

    g_allocationCount   = 0U;
    g_deallocationCount = 0U;
    g_trackAllocations  = true;
    for ( std::size_t block = 0U; block < 32U; ++block ) {
        stretcher.process(output);
    }
    g_trackAllocations = false;

    int failures = 0;
    failures += expectTrue(prepared, "prepare accepts valid format");
    failures += expectTrue(g_allocationCount == 0U,
                           "steady callback performs no heap allocation");
    failures += expectTrue(g_deallocationCount == 0U,
                           "steady callback destroys no heap objects");
    failures += expectTrue(
        std::abs(stretcher.get_actual_playback_ratio() - 0.75) < 0.001,
        "requested playback ratio becomes active");

    stretcher.set_playback_ratio(1.5);
    stretcher.set_pitch_semitones(-3.0);
    g_allocationCount   = 0U;
    g_deallocationCount = 0U;
    g_trackAllocations  = true;
    stretcher.process(output);
    g_trackAllocations = false;
    failures += expectTrue(
        std::abs(stretcher.get_actual_playback_ratio() - 1.5) < 0.001,
        "live playback ratio switch becomes active");
    failures += expectTrue(g_allocationCount == 0U,
                           "live ratio switch allocates no heap memory");
    failures += expectTrue(g_deallocationCount == 0U,
                           "live ratio switch destroys no heap objects");

    source->setAmplitude(0.0F);
    timelineEpoch.generation.store(1U, std::memory_order_release);
    g_allocationCount   = 0U;
    g_deallocationCount = 0U;
    g_trackAllocations  = true;
    stretcher.process(output);
    g_trackAllocations = false;
    failures +=
        expectTrue(stretcher.observed_provider_discontinuity_generation() == 1U,
                   "provider epoch is observed before pulling upstream");
    failures += expectTrue(g_allocationCount == 0U,
                           "provider reset allocates no heap memory");
    failures += expectTrue(g_deallocationCount == 0U,
                           "provider reset destroys no heap objects");
    failures += expectTrue(isSilent(output),
                           "provider epoch change removes previous tail");

    const std::uint64_t stateBefore = stretcher.active_state_generation();
    stretcher.set_quality(ice::TimeStretchQuality::Fast);
    failures += expectTrue(stretcher.active_state_generation() == stateBefore,
                           "prepared quality waits for block boundary");

    g_allocationCount   = 0U;
    g_deallocationCount = 0U;
    g_trackAllocations  = true;
    stretcher.process(output);
    g_trackAllocations = false;
    failures += expectTrue(stretcher.active_state_generation() > stateBefore,
                           "quality state switches at block boundary");
    failures += expectTrue(g_allocationCount == 0U,
                           "state switch does not allocate in callback");
    failures += expectTrue(g_deallocationCount == 0U,
                           "state switch defers object destruction");

    const std::uint64_t resetGeneration = stretcher.request_discontinuity();
    g_allocationCount                   = 0U;
    g_deallocationCount                 = 0U;
    g_trackAllocations                  = true;
    stretcher.process(output);
    g_trackAllocations = false;
    failures += expectTrue(
        stretcher.consumed_discontinuity_generation() == resetGeneration,
        "discontinuity is consumed at next block");
    failures += expectTrue(g_allocationCount == 0U,
                           "discontinuity reset does not allocate");
    failures += expectTrue(g_deallocationCount == 0U,
                           "discontinuity reset destroys no heap objects");
    failures +=
        expectTrue(isSilent(output), "discontinuity removes previous tail");

    source->setAmplitude(0.5F);
    for ( std::size_t block = 0U; block < 4U; ++block ) {
        stretcher.process(output);
    }
    source->setAmplitude(0.0F);
    source->advanceEpochDuringNextProcess(timelineEpoch, 2U);
    g_allocationCount   = 0U;
    g_deallocationCount = 0U;
    g_trackAllocations  = true;
    stretcher.process(output);
    g_trackAllocations = false;
    failures +=
        expectTrue(stretcher.observed_provider_discontinuity_generation() == 2U,
                   "upstream epoch change is consumed in the same callback");
    failures += expectTrue(g_allocationCount == 0U,
                           "intra-block provider reset does not allocate");
    failures += expectTrue(g_deallocationCount == 0U,
                           "intra-block provider reset does not destroy");
    failures +=
        expectTrue(isSilent(output), "intra-block reset removes previous tail");

    stretcher.collect_retired_states();
    return failures;
}

/// @brief 验证 final 请求只拉取最后一块并完整 drain。
/// @return 失败断言数量。
int testFinalDrain()
{
    constexpr ice::AudioDataFormat FORMAT{
        .channels   = 2U,
        .samplerate = 48000U,
    };
    constexpr std::size_t BLOCK_FRAMES = 128U;

    auto               source = std::make_shared<SignalNode>();
    ice::TimeStretcher stretcher;
    stretcher.set_inputnode(source);
    stretcher.set_playback_ratio(0.8);
    stretcher.prepare(FORMAT, BLOCK_FRAMES);
    ice::AudioBuffer output(FORMAT, BLOCK_FRAMES);

    stretcher.process(output);
    const std::uint64_t callsBefore     = source->callCount();
    const std::uint64_t finalGeneration = stretcher.request_final_input();

    g_allocationCount   = 0U;
    g_deallocationCount = 0U;
    g_trackAllocations  = true;
    for ( std::size_t block = 0U;
          block < 256U && !stretcher.is_final_input_drained();
          ++block ) {
        stretcher.process(output);
    }
    g_trackAllocations = false;

    int failures = 0;
    failures +=
        expectTrue(stretcher.consumed_final_generation() == finalGeneration,
                   "final request is submitted");
    failures += expectTrue(stretcher.is_final_input_drained(),
                           "final output reaches drained state");
    failures += expectTrue(source->callCount() == callsBefore + 1U,
                           "drain does not pull additional upstream blocks");
    failures += expectTrue(g_allocationCount == 0U,
                           "final and drain callbacks do not allocate");
    failures += expectTrue(g_deallocationCount == 0U,
                           "final and drain callbacks destroy no heap objects");
    return failures;
}

/// @brief 验证超过预备容量时安全静音且不临时扩容。
/// @return 失败断言数量。
int testCapacityGuard()
{
    constexpr ice::AudioDataFormat FORMAT{
        .channels   = 2U,
        .samplerate = 48000U,
    };

    auto               source = std::make_shared<SignalNode>();
    ice::TimeStretcher stretcher;
    stretcher.set_inputnode(source);
    stretcher.set_playback_ratio(1.25);
    stretcher.prepare(FORMAT, 64U);
    ice::AudioBuffer oversized(FORMAT, 65U);

    const std::uint64_t overflowsBefore = stretcher.capacity_overflow_count();
    g_allocationCount                   = 0U;
    g_deallocationCount                 = 0U;
    g_trackAllocations                  = true;
    stretcher.process(oversized);
    g_trackAllocations = false;

    int failures = 0;
    failures +=
        expectTrue(stretcher.capacity_overflow_count() == overflowsBefore + 1U,
                   "oversized block records capacity overflow");
    failures +=
        expectTrue(isSilent(oversized), "oversized block is safely silenced");
    failures += expectTrue(g_allocationCount == 0U,
                           "oversized block never grows buffers in callback");
    failures += expectTrue(g_deallocationCount == 0U,
                           "oversized block destroys no heap objects");
    return failures;
}

/// @brief 验证暂停会冻结算法、上游拉取和待切换状态。
/// @return 失败断言数量。
int testPauseFreezesPipeline()
{
    constexpr ice::AudioDataFormat FORMAT{
        .channels   = 2U,
        .samplerate = 48000U,
    };
    constexpr std::size_t BLOCK_FRAMES = 128U;

    auto               source = std::make_shared<SignalNode>();
    ice::TimeStretcher stretcher;
    stretcher.set_inputnode(source);
    stretcher.set_playback_ratio(0.75);
    stretcher.prepare(FORMAT, BLOCK_FRAMES);
    ice::AudioBuffer output(FORMAT, BLOCK_FRAMES);
    stretcher.process(output);

    stretcher.set_paused(true);
    stretcher.set_playback_ratio(1.25);
    const std::uint64_t callsBefore = source->callCount();
    const std::uint64_t stateBefore = stretcher.active_state_generation();

    beginHeapTracking();
    for ( std::size_t block = 0U; block < 8U; ++block ) {
        stretcher.process(output);
    }
    endHeapTracking();

    int failures = 0;
    failures += expectTrue(stretcher.is_paused(), "pause request is visible");
    failures += expectTrue(source->callCount() == callsBefore,
                           "pause does not pull upstream");
    failures += expectTrue(stretcher.active_state_generation() == stateBefore,
                           "pause does not consume pending state");
    failures += expectTrue(isSilent(output), "pause outputs silence");
    failures += expectNoHeapAllocation("pause callback does not allocate");
    failures += expectNoHeapDeallocation("pause callback does not free");

    stretcher.set_paused(false);
    beginHeapTracking();
    stretcher.process(output);
    endHeapTracking();
    failures += expectTrue(!stretcher.is_paused(), "resume request is visible");
    failures += expectTrue(stretcher.active_state_generation() > stateBefore,
                           "resume activates pending state at block boundary");
    failures += expectTrue(source->callCount() == callsBefore + 1U,
                           "resume continues upstream pull");
    failures += expectNoHeapAllocation("resume callback does not allocate");
    failures += expectNoHeapDeallocation("resume callback does not free");
    return failures;
}

/// @brief 验证输入帧小数余量避免长时间速度累计漂移。
/// @return 失败断言数量。
int testFractionalInputFrameRemainder()
{
    constexpr ice::AudioDataFormat FORMAT{
        .channels   = 2U,
        .samplerate = 48000U,
    };
    constexpr std::size_t BLOCK_FRAMES = 127U;
    constexpr std::size_t BLOCK_COUNT  = 1001U;
    constexpr double      RATIO        = 0.05;

    auto               source = std::make_shared<SignalNode>();
    ice::TimeStretcher stretcher;
    stretcher.set_inputnode(source);
    stretcher.set_playback_ratio(RATIO);
    stretcher.prepare(FORMAT, BLOCK_FRAMES);
    ice::AudioBuffer output(FORMAT, BLOCK_FRAMES);

    beginHeapTracking();
    for ( std::size_t block = 0U; block < BLOCK_COUNT; ++block ) {
        stretcher.process(output);
    }
    endHeapTracking();

    const auto expectedInputFrames = static_cast<std::uint64_t>(
        std::floor(static_cast<double>(BLOCK_FRAMES * BLOCK_COUNT) * RATIO));

    int failures = 0;
    failures += expectTrue(source->processedFrames() == expectedInputFrames,
                           "fractional remainder preserves cumulative ratio");
    failures +=
        expectNoHeapAllocation("fractional ratio callbacks do not allocate");
    failures +=
        expectNoHeapDeallocation("fractional ratio callbacks do not free");
    return failures;
}

/// @brief 验证极端倍率、音高及每次 reset 都保持固定容量和零堆操作。
/// @return 失败断言数量。
int testExtremePreparedStates()
{
    constexpr ice::AudioDataFormat FORMAT{
        .channels   = 2U,
        .samplerate = 48000U,
    };
    constexpr std::size_t BLOCK_FRAMES = 256U;
    struct ParameterCase {
        double                  ratio;
        double                  semitones;
        ice::TimeStretchQuality quality;
    };
    constexpr ParameterCase CASES[]{
        { 0.05, -24.0, ice::TimeStretchQuality::Fast },
        { 10.0, 24.0, ice::TimeStretchQuality::Balanced },
        { 0.05, 24.0, ice::TimeStretchQuality::Finer },
        { 10.0, -24.0, ice::TimeStretchQuality::Best },
    };

    auto               source = std::make_shared<SignalNode>();
    ice::TimeStretcher stretcher;
    stretcher.set_inputnode(source);
    stretcher.prepare(FORMAT, BLOCK_FRAMES);
    ice::AudioBuffer output(FORMAT, BLOCK_FRAMES);

    int failures = 0;
    for ( const ParameterCase& parameterCase : CASES ) {
        stretcher.set_playback_ratio(parameterCase.ratio);
        stretcher.set_pitch_semitones(parameterCase.semitones);
        stretcher.set_quality(parameterCase.quality);

        beginHeapTracking();
        stretcher.process(output);
        endHeapTracking();
        failures +=
            expectNoHeapAllocation("extreme state swap does not allocate");
        failures +=
            expectNoHeapDeallocation("extreme state swap does not free");

        const std::uint64_t resetGeneration = stretcher.request_discontinuity();
        beginHeapTracking();
        stretcher.process(output);
        endHeapTracking();
        failures += expectTrue(
            stretcher.consumed_discontinuity_generation() == resetGeneration,
            "extreme state reset is consumed");
        failures += expectNoHeapAllocation(
            "extreme reset and start padding do not allocate");
        failures +=
            expectTrue(g_deallocationCount == 0U,
                       "extreme reset does not destroy C++ heap objects");
    }

    stretcher.collect_retired_states();
    return failures;
}

/// @brief 验证 pending 参数不会截断 final 尾音且结束后不会恢复拉取。
/// @return 失败断言数量。
int testPendingStateWaitsForFinalDrain()
{
    constexpr ice::AudioDataFormat FORMAT{
        .channels   = 2U,
        .samplerate = 48000U,
    };
    constexpr std::size_t BLOCK_FRAMES = 64U;

    auto               source = std::make_shared<SignalNode>();
    ice::TimeStretcher stretcher;
    stretcher.set_inputnode(source);
    stretcher.set_playback_ratio(0.5);
    stretcher.prepare(FORMAT, BLOCK_FRAMES);
    ice::AudioBuffer output(FORMAT, BLOCK_FRAMES);
    for ( std::size_t block = 0U; block < 8U; ++block ) {
        stretcher.process(output);
    }

    const std::uint64_t finalGeneration = stretcher.request_final_input();
    stretcher.process(output);
    const std::uint64_t drainingState = stretcher.active_state_generation();
    const std::uint64_t callsAfterFinalInput = source->callCount();

    stretcher.set_playback_ratio(1.5);
    stretcher.set_pitch_semitones(7.0);
    stretcher.set_quality(ice::TimeStretchQuality::Best);

    beginHeapTracking();
    for ( std::size_t block = 0U;
          block < 2048U && !stretcher.is_final_input_drained();
          ++block ) {
        stretcher.process(output);
        if ( !stretcher.is_final_input_drained() &&
             stretcher.active_state_generation() != drainingState ) {
            endHeapTracking();
            return expectTrue(false,
                              "pending state replaced an undrained state");
        }
    }
    endHeapTracking();

    int failures = 0;
    failures +=
        expectTrue(stretcher.consumed_final_generation() == finalGeneration,
                   "pending-state final request is consumed");
    failures += expectTrue(stretcher.is_final_input_drained(),
                           "pending-state final tail is fully drained");
    failures += expectTrue(source->callCount() == callsAfterFinalInput,
                           "final drain does not pull upstream");
    failures += expectNoHeapAllocation("pending final drain does not allocate");
    failures += expectNoHeapDeallocation("pending final drain does not free");

    beginHeapTracking();
    stretcher.process(output);
    endHeapTracking();
    failures +=
        expectTrue(stretcher.active_state_generation() > drainingState,
                   "pending state activates after tail reaches terminal state");
    failures += expectTrue(source->callCount() == callsAfterFinalInput,
                           "terminal replacement remains stopped");
    failures += expectTrue(stretcher.is_final_input_drained(),
                           "terminal replacement remains drained");
    failures +=
        expectNoHeapAllocation("post-drain state activation does not allocate");
    failures +=
        expectNoHeapDeallocation("post-drain state activation does not free");

    stretcher.request_discontinuity();
    stretcher.process(output);
    failures += expectTrue(source->callCount() == callsAfterFinalInput + 1U,
                           "discontinuity reopens terminal replacement");
    return failures;
}

/// @brief 验证 RubberBand realtime padding 与 delay 在每次 reset 后恢复。
/// @return 失败断言数量。
int testRealtimeStartCompensation()
{
    constexpr ice::AudioDataFormat FORMAT{
        .channels   = 2U,
        .samplerate = 48000U,
    };
    constexpr std::size_t BLOCK_FRAMES = 64U;

    ice::RStretcher  stretcher(FORMAT,
                               ice::TimeStretchQuality::Finer,
                               BLOCK_FRAMES,
                               BLOCK_FRAMES,
                               1.25,
                               1.0);
    ice::AudioBuffer input(FORMAT, BLOCK_FRAMES);
    ice::AudioBuffer output(FORMAT, BLOCK_FRAMES);
    input.clear();

    int failures = 0;
    failures += expectTrue(stretcher.preferred_start_pad() > 0U,
                           "realtime stretcher declares start padding");
    failures += expectTrue(stretcher.start_delay() > 0U,
                           "realtime stretcher declares start delay");
    failures +=
        expectTrue(stretcher.remaining_start_delay() == stretcher.start_delay(),
                   "constructor leaves compensated stream ready");

    beginHeapTracking();
    for ( std::size_t block = 0U;
          block < 256U && stretcher.remaining_start_delay() > 0U;
          ++block ) {
        stretcher.process(output, input, false);
    }
    endHeapTracking();
    failures += expectTrue(stretcher.remaining_start_delay() == 0U,
                           "start delay is discarded across streaming calls");
    failures +=
        expectNoHeapAllocation("start-delay compensation does not allocate");
    failures +=
        expectNoHeapDeallocation("start-delay compensation does not free");

    constexpr std::size_t OUTPUT_PREFIX = 8U;
    float**               outputSamples = output.raw_ptrs();
    for ( std::uint16_t channel = 0U; channel < output.num_channels();
          ++channel ) {
        for ( std::size_t frame = 0U; frame < OUTPUT_PREFIX; ++frame ) {
            outputSamples[channel][frame] = 0.375F;
        }
    }
    beginHeapTracking();
    stretcher.process_into(output, OUTPUT_PREFIX, input, false);
    endHeapTracking();
    bool prefixPreserved = true;
    for ( std::uint16_t channel = 0U; channel < output.num_channels();
          ++channel ) {
        for ( std::size_t frame = 0U; frame < OUTPUT_PREFIX; ++frame ) {
            prefixPreserved &=
                std::abs(outputSamples[channel][frame] - 0.375F) < 0.000001F;
        }
    }
    failures += expectTrue(prefixPreserved,
                           "segment output offset preserves prior samples");
    failures +=
        expectNoHeapAllocation("segment output offset does not allocate");
    failures += expectNoHeapDeallocation(
        "segment output offset does not free heap objects");

    beginHeapTracking();
    stretcher.reset();
    endHeapTracking();
    failures +=
        expectTrue(stretcher.remaining_start_delay() == stretcher.start_delay(),
                   "reset restores full start-delay compensation");
    failures +=
        expectNoHeapAllocation("reset padding does not allocate C or C++ heap");
    failures += expectTrue(g_deallocationCount == 0U,
                           "reset padding does not delete C++ heap objects");
    return failures;
}

/// @brief 验证变速变调时同一设备 block 可跨越 loop 边界且不混用状态。
/// @return 失败断言数量。
int testStretchedLoopBoundary()
{
    constexpr ice::AudioDataFormat FORMAT{
        .channels   = 2U,
        .samplerate = 48000U,
    };
    constexpr std::size_t BLOCK_FRAMES      = 1024U;
    constexpr std::size_t RIGHT_SEGMENT     = 400U;
    constexpr std::size_t LEFT_SEGMENT      = 419U;
    constexpr std::size_t PLANNED_INPUT     = 819U;
    constexpr std::size_t MAX_OUTPUT_BLOCKS = 32U;

    auto source = std::make_shared<SegmentSignalNode>();
    source->setValues(0.6F, -0.6F);
    TestTimelineEpoch timelineEpoch;
    source->publishEpochAfterFirstPull(timelineEpoch, 1U);

    BoundaryScript boundaries;
    boundaries.add(RIGHT_SEGMENT,
                   ice::TimeStretcher::InputBoundary::Discontinuity);
    boundaries.add(LEFT_SEGMENT, ice::TimeStretcher::InputBoundary::None);

    ice::TimeStretcher stretcher;
    stretcher.set_inputnode(source);
    stretcher.set_playback_ratio(0.8);
    stretcher.set_pitch_semitones(2.0);
    stretcher.set_discontinuity_generation_provider(&timelineEpoch,
                                                    &readTimelineEpoch);
    stretcher.set_input_boundary_provider(&boundaries, &readInputBoundary);
    const bool       prepared = stretcher.prepare(FORMAT, BLOCK_FRAMES);
    ice::AudioBuffer output(FORMAT, BLOCK_FRAMES);

    float minimum = std::numeric_limits<float>::max();
    float maximum = std::numeric_limits<float>::lowest();
    beginHeapTracking();
    stretcher.process(output);
    const std::size_t callsAfterBoundaryBlock = source->callCount();
    observeExtrema(output, minimum, maximum);
    for ( std::size_t block = 1U; block < MAX_OUTPUT_BLOCKS && minimum > -0.3F;
          ++block ) {
        stretcher.process(output);
        observeExtrema(output, minimum, maximum);
    }
    endHeapTracking();

    int failures = 0;
    if ( callsAfterBoundaryBlock != 2U ||
         source->requestedFrames(1U) != LEFT_SEGMENT ) {
        std::fprintf(
            stderr,
            "Loop diagnostics: first_block_calls=%zu calls=%zu "
            "request0=%zu request1=%zu processed=%zu callbacks=%zu "
            "epoch=%llu min=%f max=%f\n",
            callsAfterBoundaryBlock,
            source->callCount(),
            source->requestedFrames(0U),
            source->requestedFrames(1U),
            source->processedFrames(),
            boundaries.callbackCount(),
            static_cast<unsigned long long>(
                stretcher.observed_provider_discontinuity_generation()),
            static_cast<double>(minimum),
            static_cast<double>(maximum));
    }
    failures += expectTrue(prepared, "loop boundary state prepares");
    failures += expectTrue(callsAfterBoundaryBlock == 2U,
                           "one device block pulls both loop sides");
    failures += expectTrue(source->requestedFrames(0U) == RIGHT_SEGMENT,
                           "right loop side stops exactly at boundary");
    failures += expectTrue(source->requestedFrames(1U) == LEFT_SEGMENT,
                           "left loop side consumes remaining block budget");
    failures += expectTrue(source->processedFrames() >= PLANNED_INPUT,
                           "loop boundary preserves all planned input");
    failures +=
        expectTrue(stretcher.observed_provider_discontinuity_generation() == 1U,
                   "explicit boundary acknowledges matching timeline epoch");
    failures +=
        expectTrue(maximum > 0.3F, "right loop side produces audible output");
    failures +=
        expectTrue(minimum < -0.3F, "left loop side produces audible output");
    failures +=
        expectNoHeapAllocation("stretched loop callbacks do not allocate");
    failures += expectTrue(g_deallocationCount == 0U,
                           "stretched loop callbacks destroy no C++ objects");
    return failures;
}

/// @brief 验证 provider Final 精确限制最后输入并完整 drain。
/// @return 失败断言数量。
int testProviderFinalBoundary()
{
    constexpr ice::AudioDataFormat FORMAT{
        .channels   = 2U,
        .samplerate = 48000U,
    };
    constexpr std::size_t BLOCK_FRAMES           = 128U;
    constexpr std::size_t FINAL_FRAMES           = 37U;
    constexpr std::size_t EXPECTED_OUTPUT_FRAMES = 49U;

    auto           source = std::make_shared<SegmentSignalNode>();
    BoundaryScript boundaries;
    boundaries.add(FINAL_FRAMES, ice::TimeStretcher::InputBoundary::Final);

    ice::TimeStretcher stretcher;
    stretcher.set_inputnode(source);
    stretcher.set_playback_ratio(0.75);
    stretcher.set_pitch_semitones(-3.0);
    stretcher.set_input_boundary_provider(&boundaries, &readInputBoundary);
    const bool       prepared = stretcher.prepare(FORMAT, BLOCK_FRAMES);
    ice::AudioBuffer output(FORMAT, BLOCK_FRAMES);

    std::size_t audibleFrames = 0U;
    beginHeapTracking();
    for ( std::size_t block = 0U;
          block < 256U && !stretcher.is_final_input_drained();
          ++block ) {
        stretcher.process(output);
        audibleFrames += countAudibleFrames(output);
    }
    const std::size_t callsWhenDrained = source->callCount();
    stretcher.process(output);
    endHeapTracking();

    int failures = 0;
    failures += expectTrue(prepared, "provider final state prepares");
    failures += expectTrue(stretcher.is_final_input_drained(),
                           "provider final reaches drained state");
    failures += expectTrue(source->callCount() == 1U,
                           "provider final pulls upstream exactly once");
    failures += expectTrue(source->requestedFrames(0U) == FINAL_FRAMES,
                           "provider final pulls exact remaining frames");
    failures += expectTrue(source->processedFrames() == FINAL_FRAMES,
                           "provider final never over-reads source");
    failures += expectTrue(
        audibleFrames > 0U && audibleFrames <= EXPECTED_OUTPUT_FRAMES,
        "provider final trims realtime block padding");
    failures += expectTrue(source->callCount() == callsWhenDrained,
                           "drained provider final remains terminal");
    failures +=
        expectNoHeapAllocation("provider final callbacks do not allocate");
    failures += expectTrue(g_deallocationCount == 0U,
                           "provider final callbacks destroy no C++ objects");
    return failures;
}

/// @brief 验证旁路会忽略 discontinuity 重置但仍严格遵守 Final。
/// @return 失败断言数量。
int testBypassBoundaryAndFinal()
{
    constexpr ice::AudioDataFormat FORMAT{
        .channels   = 2U,
        .samplerate = 48000U,
    };
    constexpr std::size_t BLOCK_FRAMES = 64U;
    constexpr std::size_t FIRST_FRAMES = 31U;
    constexpr std::size_t FINAL_FRAMES = 17U;

    auto source = std::make_shared<SegmentSignalNode>();
    source->setValues(0.6F, -0.6F);
    BoundaryScript boundaries;
    boundaries.add(FIRST_FRAMES,
                   ice::TimeStretcher::InputBoundary::Discontinuity);
    boundaries.add(FINAL_FRAMES, ice::TimeStretcher::InputBoundary::Final);

    ice::TimeStretcher stretcher;
    stretcher.set_inputnode(source);
    stretcher.set_input_boundary_provider(&boundaries, &readInputBoundary);
    const bool       prepared = stretcher.prepare(FORMAT, BLOCK_FRAMES);
    ice::AudioBuffer output(FORMAT, BLOCK_FRAMES);

    beginHeapTracking();
    stretcher.process(output);
    const std::size_t   callsAfterFinal   = source->callCount();
    const float* const* firstBlockSamples = output.raw_ptrs();
    bool                firstBlockLayout  = firstBlockSamples != nullptr;
    for ( std::size_t frame = 0U; frame < BLOCK_FRAMES && firstBlockLayout;
          ++frame ) {
        const float expected =
            frame < FIRST_FRAMES
                ? 0.6F
                : (frame < FIRST_FRAMES + FINAL_FRAMES ? -0.6F : 0.0F);
        firstBlockLayout &=
            std::abs(firstBlockSamples[0][frame] - expected) < 0.000001F;
    }
    stretcher.process(output);
    endHeapTracking();

    const bool terminalBlockSilent = isSilent(output);

    int failures = 0;
    failures += expectTrue(prepared, "bypass boundary state prepares");
    failures += expectTrue(callsAfterFinal == 2U,
                           "bypass crosses discontinuity before final");
    failures += expectTrue(source->requestedFrames(0U) == FIRST_FRAMES,
                           "bypass first segment stops at discontinuity");
    failures += expectTrue(source->requestedFrames(1U) == FINAL_FRAMES,
                           "bypass final segment has exact size");
    failures +=
        expectTrue(source->processedFrames() == FIRST_FRAMES + FINAL_FRAMES,
                   "bypass final never over-reads source");
    failures += expectTrue(firstBlockLayout,
                           "bypass preserves both boundary sides in order");
    failures += expectTrue(stretcher.is_final_input_drained(),
                           "bypass final is immediately drained");
    failures += expectTrue(source->callCount() == callsAfterFinal,
                           "bypass terminal callback does not pull upstream");
    failures += expectTrue(terminalBlockSilent,
                           "bypass terminal callback outputs silence");
    failures +=
        expectNoHeapAllocation("bypass boundary callbacks do not allocate");
    failures +=
        expectNoHeapDeallocation("bypass boundary callbacks do not free");
    return failures;
}

/// @brief 验证控制线程持续发布状态时退役回收与音频回调可安全并发。
/// @return 失败断言数量。
int testConcurrentStatePublication()
{
    constexpr ice::AudioDataFormat FORMAT{
        .channels   = 2U,
        .samplerate = 48000U,
    };
    constexpr std::size_t BLOCK_FRAMES = 128U;
    constexpr std::size_t STATE_COUNT  = 12U;

    auto               source = std::make_shared<SignalNode>();
    ice::TimeStretcher stretcher;
    stretcher.set_inputnode(source);
    stretcher.set_playback_ratio(0.75);
    const bool prepared = stretcher.prepare(FORMAT, BLOCK_FRAMES);

    std::atomic_bool         stop{ false };
    std::atomic<std::size_t> callbackCount{ 0U };
    std::atomic<std::size_t> allocationCount{ 0U };
    std::atomic<std::size_t> mallocCount{ 0U };
    std::atomic<std::size_t> deallocationCount{ 0U };
    std::thread              audioThread([&]() {
        ice::AudioBuffer output(FORMAT, BLOCK_FRAMES);
        beginHeapTracking();
        while ( !stop.load(std::memory_order_acquire) ) {
            stretcher.process(output);
            callbackCount.fetch_add(1U, std::memory_order_relaxed);
        }
        endHeapTracking();
        allocationCount.store(g_allocationCount, std::memory_order_release);
        mallocCount.store(g_mallocCount, std::memory_order_release);
        deallocationCount.store(g_deallocationCount, std::memory_order_release);
    });

    for ( std::size_t state = 0U; state < STATE_COUNT; ++state ) {
        const double ratio = state % 2U == 0U ? 0.6 : 1.4;
        const double pitch = state % 3U == 0U ? -5.0 : 4.0;
        stretcher.set_playback_ratio(ratio);
        stretcher.set_pitch_semitones(pitch);
        stretcher.set_quality(state % 2U == 0U
                                  ? ice::TimeStretchQuality::Balanced
                                  : ice::TimeStretchQuality::Finer);
    }
    stop.store(true, std::memory_order_release);
    audioThread.join();
    stretcher.collect_retired_states();

    int failures = 0;
    failures += expectTrue(prepared, "concurrent state prepares");
    failures += expectTrue(callbackCount.load(std::memory_order_acquire) > 0U,
                           "audio callback ran during state publication");
    failures +=
        expectTrue(allocationCount.load(std::memory_order_acquire) == 0U,
                   "concurrent callback performs no C++ allocation");
    failures += expectTrue(mallocCount.load(std::memory_order_acquire) == 0U,
                           "concurrent callback performs no C allocation");
    failures +=
        expectTrue(deallocationCount.load(std::memory_order_acquire) == 0U,
                   "concurrent callback destroys no C++ objects");
    return failures;
}

}  // namespace

#if defined(ICE_TEST_WRAP_MALLOC)
extern "C" void* __real_malloc(std::size_t size);
extern "C" void* __real_calloc(std::size_t count, std::size_t size);
extern "C" void* __real_realloc(void* memory, std::size_t size);
extern "C" void  __real_free(void* memory);

/// @brief 统计测试线程内的 malloc 调用并转发给真实分配器。
extern "C" void* __wrap_malloc(std::size_t size)
{
    if ( g_trackAllocations ) ++g_mallocCount;
    return __real_malloc(size);
}

/// @brief 统计测试线程内的 calloc 调用并转发给真实分配器。
extern "C" void* __wrap_calloc(std::size_t count, std::size_t size)
{
    if ( g_trackAllocations ) ++g_mallocCount;
    return __real_calloc(count, size);
}

/// @brief 统计测试线程内的 realloc 调用并转发给真实分配器。
extern "C" void* __wrap_realloc(void* memory, std::size_t size)
{
    if ( g_trackAllocations ) {
        ++g_mallocCount;
        if ( memory ) ++g_freeCount;
    }
    return __real_realloc(memory, size);
}

/// @brief 统计测试线程内的 free 调用并转发给真实分配器。
extern "C" void __wrap_free(void* memory)
{
    if ( g_trackAllocations && memory ) ++g_freeCount;
    __real_free(memory);
}
#endif

void* operator new(std::size_t size)
{
    if ( g_trackAllocations ) ++g_allocationCount;
    if ( void* memory = std::malloc(size) ) return memory;
    std::abort();
}

void* operator new[](std::size_t size)
{
    if ( g_trackAllocations ) ++g_allocationCount;
    if ( void* memory = std::malloc(size) ) return memory;
    std::abort();
}

void operator delete(void* memory) noexcept
{
    if ( g_trackAllocations ) ++g_deallocationCount;
    std::free(memory);
}

void operator delete[](void* memory) noexcept
{
    if ( g_trackAllocations ) ++g_deallocationCount;
    std::free(memory);
}

void operator delete(void* memory, std::size_t) noexcept
{
    if ( g_trackAllocations ) ++g_deallocationCount;
    std::free(memory);
}

void operator delete[](void* memory, std::size_t) noexcept
{
    if ( g_trackAllocations ) ++g_deallocationCount;
    std::free(memory);
}

int main()
{
    int failures = 0;
    failures += testRealtimeStateAndReset();
    failures += testFinalDrain();
    failures += testCapacityGuard();
    failures += testPauseFreezesPipeline();
    failures += testFractionalInputFrameRemainder();
    failures += testExtremePreparedStates();
    failures += testPendingStateWaitsForFinalDrain();
    failures += testRealtimeStartCompensation();
    failures += testStretchedLoopBoundary();
    failures += testProviderFinalBoundary();
    failures += testBypassBoundaryAndFinal();
    failures += testConcurrentStatePublication();
    return failures == 0 ? 0 : 1;
}
