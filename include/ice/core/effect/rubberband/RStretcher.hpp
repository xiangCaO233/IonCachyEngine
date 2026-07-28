#pragma once

#include "ice/manage/AudioBuffer.hpp"
#include "ice/manage/AudioFormat.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace RubberBand
{
class RubberBandStretcher;
}

namespace ice
{

/// @brief RubberBand 实时时间拉伸的质量档位。
enum class TimeStretchQuality { Fast, Balanced, Finer, Best };

/// @brief 预分配的 RubberBand 实时流式处理包装。
class RStretcher
{
public:
    /// @brief 使用引擎默认 block 容量构造并预热实时拉伸器。
    /// @param format 固定音频格式。
    /// @param quality 质量档位。
    explicit RStretcher(const AudioDataFormat& format,
                        TimeStretchQuality quality = TimeStretchQuality::Finer);

    /// @brief 构造并预热实时拉伸器。
    /// @param format 固定音频格式。
    /// @param quality 质量档位。
    /// @param maxInputFrames 单次上层调用可能提供的最大输入帧数。
    /// @param maxOutputFrames 单次上层调用可能请求的最大输出帧数。
    /// @param initialStretchRatio 初始时间拉伸倍率。
    /// @param initialPitchRatio 初始音高倍率。
    RStretcher(const AudioDataFormat& format, TimeStretchQuality quality,
               std::size_t maxInputFrames, std::size_t maxOutputFrames,
               double initialStretchRatio = 1.0,
               double initialPitchRatio   = 1.0);

    /// @brief 析构 RubberBand 状态。
    ~RStretcher();

    RStretcher(const RStretcher&)            = delete;
    RStretcher& operator=(const RStretcher&) = delete;
    RStretcher(RStretcher&&)                 = delete;
    RStretcher& operator=(RStretcher&&)      = delete;

    /// @brief 更新时间拉伸倍率。
    /// @param ratio 输出时长与输入时长之比。
    /// @warning 兼容性控制接口：RubberBand 可能重新配置内部状态，不得从
    /// 音频回调调用；实时路径应构造带最终倍率的新 RStretcher。
    void set_stretch_ratio(double ratio);

    /// @brief 更新音高倍率。
    /// @param pitchRatio 目标频率与输入频率之比。
    /// @warning 兼容性控制接口：RubberBand 可能重新配置内部状态，不得从
    /// 音频回调调用；实时路径应构造带最终倍率的新 RStretcher。
    void set_pitch_ratio(double pitchRatio);

    /// @brief 获取当前时间拉伸倍率。
    /// @return 输出时长与输入时长之比。
    [[nodiscard]] double get_stretch_ratio() const;

    /// @brief 获取当前音高倍率。
    /// @return 目标频率与输入频率之比。
    [[nodiscard]] double get_pitch_ratio() const;

    /// @brief 清空算法历史并恢复可继续输入的状态。
    /// @warning 音频回调热路径：不得在此引入分配或销毁算法对象。
    void reset();

    /// @brief 将一块输入送入拉伸器并提取当前可用输出。
    /// @param output 固定容量的输出缓冲。
    /// @param input 输入缓冲。
    /// @param finalInput 此块是否为本段流的最后输入。
    /// @return 写入输出缓冲的帧数。
    /// @warning 音频回调热路径：不分配内存，不创建临时容器。
    std::size_t process(AudioBuffer& output, const AudioBuffer& input,
                        bool finalInput = false);

    /// @brief 将单段输入送入拉伸器，并从指定输出偏移开始提取。
    /// @param output 固定容量的输出缓冲。
    /// @param outputOffset 保留不改写的输出前缀帧数。
    /// @param input 单个连续时间线区间的输入缓冲。
    /// @param finalInput 此段是否为当前算法流的最后输入。
    /// @return 从 outputOffset 起写入的帧数，不清理其余输出区域。
    /// @warning 音频回调热路径：用于边界拆段，且不分配内存。
    std::size_t process_into(AudioBuffer& output, std::size_t outputOffset,
                             const AudioBuffer& input, bool finalInput = false);

    /// @brief 不再送入数据，仅提取结束刷新产生的剩余输出。
    /// @param output 固定容量的输出缓冲。
    /// @return 写入输出缓冲的帧数。
    /// @warning 音频回调热路径：不分配内存。
    std::size_t drain(AudioBuffer& output);

    /// @brief 从指定输出偏移开始提取结束刷新产生的剩余输出。
    /// @param output 固定容量的输出缓冲。
    /// @param outputOffset 保留不改写的输出前缀帧数。
    /// @return 从 outputOffset 起写入的帧数，不清理其余输出区域。
    /// @warning 音频回调热路径：不分配内存。
    std::size_t drain_into(AudioBuffer& output, std::size_t outputOffset);

    /// @brief 查询结束刷新是否已经完全读空。
    /// @return RubberBand 已报告结束时返回 true。
    [[nodiscard]] bool is_finished() const;

    /// @brief 查询包装器固定的音频格式。
    /// @return 固定音频格式。
    [[nodiscard]] const AudioDataFormat& format() const;

    /// @brief 获取当前倍率对应的实时起始填充帧数。
    /// @return 每次 reset 后自动提交的静音输入帧数。
    [[nodiscard]] std::size_t preferred_start_pad() const;

    /// @brief 获取当前倍率对应的实时输出起始延迟。
    /// @return 每次 reset 后需要从输出丢弃的帧数。
    [[nodiscard]] std::size_t start_delay() const;

    /// @brief 获取本段尚未丢弃的起始延迟。
    /// @return 后续 retrieve 仍需丢弃的帧数。
    [[nodiscard]] std::size_t remaining_start_delay() const;

private:
    /// @brief 构造 RubberBand 选项。
    /// @param quality 质量档位。
    /// @return RubberBand 选项位。
    [[nodiscard]] static int makeOptions(TimeStretchQuality quality);

    /// @brief 在控制线程执行一次最大块预热。
    /// @param maxInputFrames 预热输入帧数。
    /// @param maxOutputFrames 预热输出帧数。
    void prewarm(std::size_t maxInputFrames, std::size_t maxOutputFrames);

    /// @brief 在 reset 后提交 RubberBand 要求的静音起始填充。
    /// @warning 音频回调热路径：只使用固定容量缓冲和指针数组。
    void primeStart();

    /// @brief 从 RubberBand 队列丢弃尚未补偿的起始延迟。
    /// @warning 音频回调热路径：只写入固定容量丢弃缓冲。
    void discardStartDelay();

    /// @brief 从 RubberBand 队列提取到输出指定位置。
    /// @param output 输出缓冲。
    /// @param outputOffset 输出起始帧。
    /// @return 本次提取帧数。
    /// @warning 音频回调热路径：复用预分配的声道指针数组。
    std::size_t retrieveAvailable(AudioBuffer& output,
                                  std::size_t  outputOffset);

    /// @brief 固定音频格式。
    AudioDataFormat m_format;

    /// @brief 当前时间拉伸倍率，仅由音频线程读写。
    double m_stretchRatio{ 1.0 };

    /// @brief 当前音高倍率，仅由音频线程读写。
    double m_pitchRatio{ 1.0 };

    /// @brief RubberBand 每次 process 接受的最大帧数。
    std::size_t m_maxProcessFrames{ 0U };

    /// @brief 构造时允许的最大输入帧数。
    std::size_t m_maxInputFrames{ 0U };

    /// @brief 构造时允许的最大输出帧数。
    std::size_t m_maxOutputFrames{ 0U };

    /// @brief 复用的输入声道偏移指针。
    std::vector<const float*> m_inputPointers;

    /// @brief 复用的输出声道偏移指针。
    std::vector<float*> m_outputPointers;

    /// @brief reset 时用于提交起始 padding 的固定静音缓冲。
    AudioBuffer m_paddingInput;

    /// @brief 丢弃起始 delay 时使用的固定输出缓冲。
    AudioBuffer m_delayDiscardOutput;

    /// @brief 当前参数对应的输入起始 padding。
    std::size_t m_preferredStartPad{ 0U };

    /// @brief 当前参数对应的输出起始 delay。
    std::size_t m_startDelay{ 0U };

    /// @brief 当前段仍待跨 block 丢弃的起始 delay。
    std::size_t m_remainingStartDelay{ 0U };

    /// @brief RubberBand 是否已完成结束刷新。
    bool m_finished{ false };

    /// @brief RubberBand 算法对象。
    std::unique_ptr<RubberBand::RubberBandStretcher> m_rubberBandStretcher;
};

}  // namespace ice
