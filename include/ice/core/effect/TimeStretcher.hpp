#pragma once

#include "ice/core/effect/IEffectNode.hpp"
#include "ice/core/effect/rubberband/RStretcher.hpp"
#include "ice/manage/AudioFormat.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace ice
{

/// @brief 支持实时安全状态切换的播放速度与音高处理节点。
class TimeStretcher : public IEffectNode
{
public:
    /// @brief 一段输入末端需要执行的时间线动作。
    enum class InputBoundary {
        None,
        Discontinuity,
        Final,
    };

    /// @brief 输入连续区间查询结果。
    struct InputSpan {
        /// @brief 当前连续区间允许拉取的输入帧数。
        std::size_t frameCount{ 0U };

        /// @brief 拉取 frameCount 帧后需要执行的边界动作。
        InputBoundary boundary{ InputBoundary::None };
    };

    /// @brief 不持有上下文的轻量 discontinuity 代际读取函数。
    using DiscontinuityGenerationReader =
        std::uint64_t (*)(const void* context) noexcept;

    /// @brief 不持有上下文的输入连续区间查询函数。
    using InputBoundaryReader =
        InputSpan (*)(void* context, std::size_t maxInputFrames) noexcept;

    /// @brief 使用引擎默认格式和 block 容量构造并预热节点。
    TimeStretcher();

    /// @brief 回收全部预备与退役处理状态。
    /// @warning 析构前必须先停止音频回调。
    ~TimeStretcher() override;

    TimeStretcher(const TimeStretcher&)            = delete;
    TimeStretcher& operator=(const TimeStretcher&) = delete;
    TimeStretcher(TimeStretcher&&)                 = delete;
    TimeStretcher& operator=(TimeStretcher&&)      = delete;

    /// @brief 从上游拉取并输出一块变速音频。
    /// @param buffer 输出缓冲。
    /// @warning 音频回调热路径：不持锁、不分配、不销毁处理状态。
    void process(AudioBuffer& buffer) override;

    /// @brief 在控制线程预备格式、最大 block 和 RubberBand 状态。
    /// @param format 音频回调使用的固定格式。
    /// @param maxOutputFrames 单次回调可能请求的最大输出帧数。
    /// @return 参数有效且状态已发布时返回 true。
    /// @warning 低频控制路径：会分配并预热
    /// RubberBand，且不得与另一个控制线程并发调用。
    bool prepare(const AudioDataFormat& format, std::size_t maxOutputFrames);

    /// @brief 设置期望的播放速度倍率。
    /// @param desiredRatio 播放速度倍率，范围为 0.05 到 10.0。
    /// @warning 低频控制路径：参数变化会构造并预热完整 RubberBand 状态，
    /// 不得从音频回调调用。
    void set_playback_ratio(double desiredRatio);

    /// @brief 设置期望的音高偏移。
    /// @param semitones 半音偏移，范围为 -24 到 24。
    /// @warning 低频控制路径：参数变化会构造并预热完整 RubberBand 状态，
    /// 不得从音频回调调用。
    void set_pitch_semitones(double semitones);

    /// @brief 获取最近一次回调实际使用的播放速度倍率。
    /// @return 输入帧数与输出帧数之比。
    [[nodiscard]] double get_actual_playback_ratio() const;

    /// @brief 获取期望的音高偏移。
    /// @return 半音偏移。
    [[nodiscard]] double get_pitch_semitones() const;

    /// @brief 切换质量并在控制线程构建替换状态。
    /// @param quality 目标质量档位。
    /// @warning 低频控制路径：会分配并预热 RubberBand。
    void set_quality(TimeStretchQuality quality);

    /// @brief 获取当前要求的质量档位。
    /// @return 质量档位。
    [[nodiscard]] TimeStretchQuality get_quality() const;

    /// @brief 冻结或恢复算法及其上游输入。
    /// @param paused true 时后续回调只输出静音，且不消费任何状态请求。
    /// @warning 无锁控制接口：恢复后在下一个 block 边界继续原算法历史。
    void set_paused(bool paused) noexcept;

    /// @brief 查询当前暂停要求。
    /// @return 已要求暂停时返回 true。
    [[nodiscard]] bool is_paused() const noexcept;

    /// @brief 请求在下一音频 block 边界清空算法历史。
    /// @return 本次请求的单调递增代际。
    /// @warning 无锁邮箱：可由控制线程在 Seek 或循环 epoch 改变时调用。
    std::uint64_t request_discontinuity() noexcept;

    /// @brief 获取音频线程已经应用的 discontinuity 代际。
    /// @return 最近已应用代际。
    [[nodiscard]] std::uint64_t consumed_discontinuity_generation() const;

    /// @brief 绑定每个 block 开始时读取的外部 discontinuity 代际。
    /// @param context 生命周期覆盖全部音频回调的非拥有上下文。
    /// @param reader 无异常、无阻塞、无分配的代际读取函数。
    /// @warning 控制线程接口；替换时旧 context 仍须保持有效直到音频停止。
    void set_discontinuity_generation_provider(
        const void* context, DiscontinuityGenerationReader reader);

    /// @brief 清除外部 discontinuity 代际读取器。
    /// @warning 控制线程接口；不会影响手动 request_discontinuity 邮箱。
    void clear_discontinuity_generation_provider();

    /// @brief 获取音频线程最近观察到的外部代际。
    /// @return 最近稳定读取的代际。
    [[nodiscard]] std::uint64_t
    observed_provider_discontinuity_generation() const;

    /// @brief 绑定每次上游拉取前调用的输入连续区间查询器。
    /// @param context 生命周期覆盖全部音频回调的非拥有上下文。
    /// @param reader 返回不超过 maxInputFrames 的连续输入帧数及段尾动作。
    /// @warning 控制线程接口；reader 必须无异常、无阻塞且不分配内存。
    void set_input_boundary_provider(void* context, InputBoundaryReader reader);

    /// @brief 清除输入连续区间查询器。
    /// @warning 控制线程接口；后续每个 block 恢复为单段拉取。
    void clear_input_boundary_provider();

    /// @brief 声明下一块上游输入是本段流的最后一块。
    /// @return 本次请求的单调递增代际。
    /// @warning 无锁邮箱：结束后节点会继续 drain，而不会再次拉取上游。
    std::uint64_t request_final_input() noexcept;

    /// @brief 获取音频线程已经提交给 RubberBand 的 final 代际。
    /// @return 最近已提交代际。
    [[nodiscard]] std::uint64_t consumed_final_generation() const;

    /// @brief 查询 final 输出是否已经完全 drain。
    /// @return 本段非直通流已经完全输出时返回 true。
    [[nodiscard]] bool is_final_input_drained() const;

    /// @brief 回收音频线程已经退役的 RubberBand 状态。
    /// @warning 低频控制路径：只销毁音频线程已明确退役的不同实例，
    /// 不得从音频回调调用，也不得与另一个控制线程并发。
    void collect_retired_states();

    /// @brief 获取音频线程已激活的预备状态代际。
    /// @return 最近已激活代际。
    [[nodiscard]] std::uint64_t active_state_generation() const;

    /// @brief 获取因 block 超出预备容量而静音的次数。
    /// @return 容量溢出次数。
    [[nodiscard]] std::uint64_t capacity_overflow_count() const;

protected:
    /// @brief 对已经拉取的输入应用当前 RubberBand 状态。
    /// @param output 输出缓冲。
    /// @param input 输入缓冲。
    /// @warning 音频回调热路径：不持锁、不分配。
    void apply_effect(AudioBuffer& output, const AudioBuffer& input) override;

private:
    struct ProcessingState;

    /// @brief 播放速度允许的下限。
    static constexpr double MIN_PLAYBACK_RATIO = 0.05;

    /// @brief 播放速度允许的上限。
    static constexpr double MAX_PLAYBACK_RATIO = 10.0;

    /// @brief 在控制线程创建并发布一份预热状态。
    /// @param format 固定音频格式。
    /// @param maxOutputFrames 最大输出 block 帧数。
    /// @param quality 质量档位。
    /// @return 状态成功发布时返回 true。
    bool publish_prepared_state(const AudioDataFormat& format,
                                std::size_t            maxOutputFrames,
                                TimeStretchQuality     quality);

    /// @brief 在 block 边界接收最新预热状态。
    /// @warning 音频回调热路径：仅交换指针，旧状态延迟到控制线程回收。
    void apply_pending_state();

    /// @brief 将旧状态压入无锁退役链。
    /// @param state 已不再被音频线程使用的状态。
    /// @warning 音频回调热路径：不销毁状态。
    void retire_state(ProcessingState* state);

    /// @brief 应用 Seek 或循环造成的历史重置请求。
    /// @param resetStretcher 是否需要重置非旁路 RubberBand。
    /// @return 本次调用消费了新请求时返回 true。
    /// @warning 音频回调热路径：只重置当前预热状态。
    bool apply_discontinuity_request(bool resetStretcher);

    /// @brief 读取外部代际并在变化时重置历史。
    /// @param resetStretcher 是否需要重置非旁路 RubberBand。
    /// @return 本次调用观察并应用了新代际时返回 true。
    /// @warning 音频回调热路径：只读原子和轻量函数指针，不分配内存。
    bool apply_provider_discontinuity(bool resetStretcher);

    /// @brief 清空当前状态的流式历史和结束标志。
    /// @param resetStretcher 是否调用 RubberBand reset。
    /// @warning 音频回调热路径：只重置已经预热的算法状态。
    void reset_processing_history(bool resetStretcher);

    /// @brief 稳定读取输入连续区间 provider 并校验结果。
    /// @param maxInputFrames 当前仍计划拉取的最大输入帧数。
    /// @return 可安全处理的连续输入区间。
    /// @warning 音频回调热路径：只读原子并调用轻量函数指针。
    [[nodiscard]] InputSpan read_input_span(std::size_t maxInputFrames) const;

    /// @brief 分段执行无变速旁路。
    /// @param output 输出 block。
    /// @param inputNode 上游节点。
    /// @param inputFrames 当前 block 计划拉取的总输入帧。
    /// @param finalAtBlockEnd 旧 final 邮箱是否要求在 block 末结束。
    /// @param initialOutputOffset 当前 block 已由旧段 drain 填充的帧数。
    /// @return 实际从上游拉取的输入帧数。
    /// @warning 音频回调热路径：只使用 ProcessingState 固定 scratch。
    [[nodiscard]] std::size_t process_bypass_segments(
        AudioBuffer& output, IAudioNode& inputNode, std::size_t inputFrames,
        bool finalAtBlockEnd, std::size_t initialOutputOffset);

    /// @brief 分段执行 RubberBand 路径。
    /// @param output 输出 block。
    /// @param inputNode 上游节点。
    /// @param inputFrames 当前 block 计划拉取的总输入帧。
    /// @param finalAtBlockEnd 旧 final 邮箱是否要求在 block 末结束。
    /// @param initialOutputOffset 当前 block 已由旧段 drain 填充的帧数。
    /// @return 实际从上游拉取的输入帧数。
    /// @warning 音频回调热路径：不分配并保持边界两侧状态隔离。
    [[nodiscard]] std::size_t process_stretched_segments(
        AudioBuffer& output, IAudioNode& inputNode, std::size_t inputFrames,
        bool finalAtBlockEnd, std::size_t initialOutputOffset);

    /// @brief 将当前状态标记为 final 并发布 drain 状态。
    /// @param finalGeneration 当前 final 请求代际。
    /// @warning 音频回调热路径：仅写当前状态与原子状态。
    void publish_final_state(std::uint64_t finalGeneration);

    /// @brief 判断当前参数是否可以直接旁路。
    /// @param playbackRatio 播放速度。
    /// @param pitchSemitones 半音偏移。
    /// @return 无需 RubberBand 时返回 true。
    [[nodiscard]] static bool should_bypass(double playbackRatio,
                                            double pitchSemitones);

    /// @brief 期望播放速度。
    std::atomic<double> m_desiredPlaybackRatio{ 1.0 };

    /// @brief 期望音高半音偏移。
    std::atomic<double> m_desiredPitchSemitones{ 0.0 };

    /// @brief 最近实际播放速度。
    std::atomic<double> m_actualPlaybackRatio{ 1.0 };

    /// @brief 控制线程要求的质量。
    std::atomic<TimeStretchQuality> m_quality{ TimeStretchQuality::Finer };

    /// @brief 控制线程要求的暂停状态。
    std::atomic_bool m_paused{ false };

    /// @brief 控制线程记录的预备格式。
    AudioDataFormat m_controlFormat;

    /// @brief 控制线程记录的最大输出 block。
    std::size_t m_controlMaxOutputFrames{ 0U };

    /// @brief 音频线程独占的当前处理状态。
    ProcessingState* m_currentState{ nullptr };

    /// @brief 控制线程发布、音频线程在 block 边界接收的状态邮箱。
    std::atomic<ProcessingState*> m_pendingState{ nullptr };

    /// @brief 音频线程发布、控制线程回收的退役状态链。
    std::atomic<ProcessingState*> m_retiredStates{ nullptr };

    /// @brief 控制线程分配给新状态的代际。
    std::uint64_t m_nextStateGeneration{ 1U };

    /// @brief 音频线程已激活的状态代际。
    std::atomic<std::uint64_t> m_activeStateGeneration{ 0U };

    /// @brief 控制线程提交的 discontinuity 请求代际。
    std::atomic<std::uint64_t> m_requestedDiscontinuityGeneration{ 0U };

    /// @brief 音频线程已应用的 discontinuity 代际。
    std::atomic<std::uint64_t> m_consumedDiscontinuityGeneration{ 0U };

    /// @brief 外部 provider 配置的 seqlock 序号。
    std::atomic<std::uint64_t> m_providerConfigurationSequence{ 0U };

    /// @brief 外部 provider 的非拥有上下文。
    std::atomic<const void*> m_discontinuityProviderContext{ nullptr };

    /// @brief 外部 provider 的轻量读取函数。
    std::atomic<DiscontinuityGenerationReader> m_discontinuityGenerationReader{
        nullptr
    };

    /// @brief 输入边界 provider 配置的 seqlock 序号。
    std::atomic<std::uint64_t> m_inputBoundaryConfigurationSequence{ 0U };

    /// @brief 输入边界 provider 的非拥有上下文。
    std::atomic<void*> m_inputBoundaryProviderContext{ nullptr };

    /// @brief 输入边界 provider 的轻量查询函数。
    std::atomic<InputBoundaryReader> m_inputBoundaryReader{ nullptr };

    /// @brief 音频线程私有的最近外部代际。
    std::uint64_t m_observedProviderGeneration{ 0U };

    /// @brief 是否已经读取过当前 provider 的初始代际。
    bool m_hasObservedProviderGeneration{ false };

    /// @brief 供控制线程诊断的最近外部代际。
    std::atomic<std::uint64_t> m_publishedProviderGeneration{ 0U };

    /// @brief 控制线程提交的 final 请求代际。
    std::atomic<std::uint64_t> m_requestedFinalGeneration{ 0U };

    /// @brief 音频线程已提交给 RubberBand 的 final 代际。
    std::atomic<std::uint64_t> m_consumedFinalGeneration{ 0U };

    /// @brief 当前 final 流是否已经完全 drain。
    std::atomic_bool m_finalInputDrained{ false };

    /// @brief 超出预备容量的回调数量。
    std::atomic<std::uint64_t> m_capacityOverflowCount{ 0U };
};

static_assert(std::atomic<TimeStretcher*>::is_always_lock_free);
static_assert(
    std::atomic<
        TimeStretcher::DiscontinuityGenerationReader>::is_always_lock_free);
static_assert(
    std::atomic<TimeStretcher::InputBoundaryReader>::is_always_lock_free);
static_assert(std::atomic<double>::is_always_lock_free);
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
static_assert(std::atomic_bool::is_always_lock_free);

}  // namespace ice
