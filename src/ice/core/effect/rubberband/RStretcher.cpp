#include "ice/core/effect/rubberband/RStretcher.hpp"

#include "ice/config/config.hpp"

#include <rubberband/RubberBandStretcher.h>

#include <algorithm>
#include <cmath>
#include <limits>

namespace ice
{
namespace
{

/// @brief 将非法倍率归一为安全值。
/// @param ratio 待检查倍率。
/// @return 可交给 RubberBand 的正有限倍率。
[[nodiscard]] double sanitizeRatio(double ratio)
{
    // 时长/音高倍率必须为正且有限；零、负值和 NaN 不能参与容量估算。
    // 保留所有合法正值，不在这个兼容层另设与上层不同的倍率限幅。
    if ( !std::isfinite(ratio) || ratio <= 0.0 ) return 1.0;
    return ratio;
}

}  // namespace

/// @brief 使用引擎默认块大小委托给完整构造路径。
/// @param format 所有后续输入输出必须保持的格式。
/// @param quality 预热前一次性确定的质量档位。
/// @warning 该便利构造同样执行完整预热，不能因参数较少而从实时线程调用。
RStretcher::RStretcher(const AudioDataFormat& format,
                       TimeStretchQuality     quality)
    : RStretcher(
          format, quality,
          std::max<std::size_t>(
              1U, static_cast<std::size_t>(ICEConfig::default_buffer_size)),
          std::max<std::size_t>(
              1U, static_cast<std::size_t>(ICEConfig::default_buffer_size)))
{
}

/// @brief 创建固定格式的实时处理器并覆盖首次处理、结束刷新和重置路径。
/// @param format 输入输出共用的平面浮点格式，不在处理期间自动重采样。
/// @param quality 窗长和音高处理策略的组合，不改变输入输出格式。
/// @param maxInputFrames 上层预期的最大单次输入，用于控制阶段预热。
/// @param maxOutputFrames 输出工作区的预备容量，以帧而非字节为单位。
/// @param initialStretchRatio 输出时长与输入时长之比。
/// @param initialPitchRatio 目标频率与原频率之比。
/// @warning 控制线程低频路径，构造与预热会分配并同步处理静音数据。
RStretcher::RStretcher(const AudioDataFormat& format,
                       TimeStretchQuality quality, std::size_t maxInputFrames,
                       std::size_t maxOutputFrames, double initialStretchRatio,
                       double initialPitchRatio)
    : m_format(format)
    , m_stretchRatio(sanitizeRatio(initialStretchRatio))
    , m_pitchRatio(sanitizeRatio(initialPitchRatio))
    , m_maxInputFrames(std::max<std::size_t>(1U, maxInputFrames))
    , m_maxOutputFrames(std::max<std::size_t>(1U, maxOutputFrames))
    , m_inputPointers(format.channels, nullptr)
    , m_outputPointers(format.channels, nullptr)
{
    // 内部指针数组与声道数绑定；process 只能改指针指向，不能扩充数组。
    // 先按最终倍率构造，使预热覆盖将要发布给音频线程的实际配置。
    m_rubberBandStretcher =
        std::make_unique<RubberBand::RubberBandStretcher>(format.samplerate,
                                                          format.channels,
                                                          makeOptions(quality),
                                                          m_stretchRatio,
                                                          m_pitchRatio);

    // 输入补零与输出丢弃是两个方向的补偿，不能把它们当成同一帧数。
    // 此处缓存构造配置的补偿量，reset 会在每个新算法流重新应用。
    m_preferredStartPad = m_rubberBandStretcher->getPreferredStartPad();
    m_startDelay        = m_rubberBandStretcher->getStartDelay();

    const std::size_t processLimit =
        m_rubberBandStretcher->getProcessSizeLimit();
    // 预备容量既要覆盖正常输入，也要覆盖输出需求与首段静音输入。
    // 最终仍受后端单次 process 上限约束，超出的输入通过分块发送。
    const std::size_t reservedProcessFrames =
        std::max({ m_maxInputFrames,
                   m_maxOutputFrames,
                   std::max<std::size_t>(1U, m_preferredStartPad) });
    m_maxProcessFrames = std::max<std::size_t>(
        1U, std::min(reservedProcessFrames, processLimit));
    m_rubberBandStretcher->setMaxProcessSize(m_maxProcessFrames);
    // 单次处理上限与上层输出容量不等价：拉伸后可能多次 retrieve 才能排空。

    // padding 可分多次提交，不必为整段前补零分配同样大的缓冲。
    // delay 丢弃另用输出侧工作区，不能覆盖调用者已写入的样本。
    m_paddingInput.resize(
        m_format,
        std::max<std::size_t>(
            1U, std::min(m_preferredStartPad, m_maxProcessFrames)));
    m_delayDiscardOutput.resize(m_format, m_maxOutputFrames);
    m_paddingInput.clear();
    m_delayDiscardOutput.clear();

    // 预热结束会再次 reset，构造后的实例仍可从真实流的第一帧开始。
    prewarm(m_maxInputFrames, m_maxOutputFrames);
}

/// @brief 释放后端及预分配工作区。
/// @warning 销毁必须在不再有处理调用时执行，不能由实时回调承担对象回收。
RStretcher::~RStretcher() = default;

/// @brief 兼容性倍率更新接口，保留合法有限倍率。
/// @param ratio 输出时长相对于输入的倍率，不是播放速度倍率。
/// @warning 可能触发后端重配置；不得与处理并发，实时切换应发布预热后的新实例。
void RStretcher::set_stretch_ratio(double ratio)
{
    const double sanitized = sanitizeRatio(ratio);
    // 无实质变化时不触碰后端，避免重复设置导致内部状态扰动。
    if ( std::abs(sanitized - m_stretchRatio) <=
         std::numeric_limits<double>::epsilon() ) {
        return;
    }
    m_stretchRatio = sanitized;
    // 此兼容入口不重新查询启动补偿量，也不重做预热。
    // 对实时处理有严格要求的调用者应按新参数创建并发布独立实例。
    m_rubberBandStretcher->setTimeRatio(sanitized);
}

/// @brief 在控制路径更新独立音高比例，不改变固定声道数和采样率。
/// @param pitchRatio 频率倍率；非法值回到单位倍率。
/// @warning 此接口不重建包装器缓存的 padding/delay，运行时应优先换用新实例。
void RStretcher::set_pitch_ratio(double pitchRatio)
{
    const double sanitized = sanitizeRatio(pitchRatio);
    // 音高使用独立倍率缓存，不能以修改时长倍率来模拟音高设置。
    if ( std::abs(sanitized - m_pitchRatio) <=
         std::numeric_limits<double>::epsilon() ) {
        return;
    }
    m_pitchRatio = sanitized;
    m_rubberBandStretcher->setPitchScale(sanitized);
}

/// @brief 返回最后设置并归一化的时长倍率，而非后端瞬时产出比。
/// @warning 普通成员读取，不提供跨线程参数发布保证。
double RStretcher::get_stretch_ratio() const
{
    return m_stretchRatio;
}

/// @brief 返回音高配置值；不根据采样内容重新估计频率。
/// @warning 不得与控制侧参数写入并发。
double RStretcher::get_pitch_ratio() const
{
    return m_pitchRatio;
}

/// @brief 清除上一段的历史和终止状态，为新算法流重新补偿起点。
/// @warning 音频热路径可能调用；不分配新的工作区，不销毁算法实例。
void RStretcher::reset()
{
    // 后端 reset 不代表包装层也已恢复，结束标记与剩余 delay 必须一起重置。
    m_rubberBandStretcher->reset();
    m_finished            = false;
    m_remainingStartDelay = m_startDelay;
    // 静音前缀只属于算法准备，不计入调用方的真实输入帧数。
    primeStart();
}

/// @brief 从输出起点处理一块输入，并将未产出的尾部静音。
/// @return 本次有效输出帧数，不包含补零尾部。
/// @warning 音频周期热路径，只复用缓冲；返回零不必然意味着流结束。
std::size_t RStretcher::process(AudioBuffer& output, const AudioBuffer& input,
                                bool finalInput)
{
    // 便利接口独占整块输出，与保留前后区域的 process_into 区分开。
    const std::size_t written = process_into(output, 0U, input, finalInput);
    // 只有实际产出的前缀有效；尾部必须清零以免播放上次缓冲残留。
    if ( written < output.num_frames() ) output.clear_from(written);
    return written;
}

/// @brief 消费输入并把当前可取样本追加到指定输出偏移。
/// @param output 已预备的输出工作区，禁止在这里扩容。
/// @param outputOffset 以帧为单位的写入起点，前缀不改动。
/// @param input 本次全部输入；输出满后其余产出仍留在后端队列。
/// @param finalInput 只对最后一个输入分块发送终止标记。
/// @return 实际写入帧数，不等于本次消费的输入帧数。
/// @warning
/// 实时热路径，不分配、不等待输入，不可与参数更新或另一个处理调用并发。
std::size_t RStretcher::process_into(AudioBuffer&       output,
                                     std::size_t        outputOffset,
                                     const AudioBuffer& input, bool finalInput)
{
    // 对齐格式、指针数组与活动帧边界；拒绝后不推进后端状态。
    // m_finished 表示后端已终止，必须 reset 才能提交下一段真实输入。
    if ( output.afmt != m_format || input.afmt != m_format ||
         input.num_channels() != m_inputPointers.size() ||
         output.num_channels() != m_outputPointers.size() ||
         outputOffset > output.num_frames() || m_finished ) {
        return 0U;
    }

    const float* const* inputChannels = input.raw_ptrs();
    // AudioBuffer 的有效格式和活动帧数契约包含平面存储可读性。
    // 本接口不接收裸 PCM 字节流，也不负责外部指针所有权。
    const std::size_t inputFrames   = input.num_frames();
    std::size_t       inputOffset   = 0U;
    std::size_t       writtenFrames = 0U;

    while ( inputOffset < inputFrames ) {
        // 只切分输入，不复制音频数据；各声道使用相同帧偏移保持相位对应。
        const std::size_t chunkFrames =
            std::min(m_maxProcessFrames, inputFrames - inputOffset);
        for ( std::uint16_t channel = 0U; channel < m_format.channels;
              ++channel ) {
            m_inputPointers[channel] = inputChannels[channel] + inputOffset;
        }

        // 不能把 final 标在中间分块，否则后续输入会落到已经结束的算法流。
        const bool isFinalChunk =
            finalInput && inputOffset + chunkFrames == inputFrames;
        m_rubberBandStretcher->process(
            m_inputPointers.data(), chunkFrames, isFinalChunk);
        inputOffset += chunkFrames;

        if ( outputOffset + writtenFrames < output.num_frames() ) {
            // 边送边取减少后端积压；一次输入可以暂时没有任何可取输出。
            writtenFrames +=
                retrieveAvailable(output, outputOffset + writtenFrames);
        }
    }

    if ( inputFrames == 0U && finalInput ) {
        // 上层可能在前一块末尾才得知 EOF，用零帧 final 完成同一结束协议。
        m_rubberBandStretcher->process(m_inputPointers.data(), 0U, true);
    }
    if ( outputOffset + writtenFrames < output.num_frames() ) {
        // 末次提交或零输入调用后仍可能有可用尾部，额外尝试一次提取。
        writtenFrames +=
            retrieveAvailable(output, outputOffset + writtenFrames);
    }

    // available 为零只是暂时缺输出，负值才表示结束刷新已经彻底读空。
    const int available = m_rubberBandStretcher->available();
    if ( available < 0 ) m_finished = true;
    return writtenFrames;
}

/// @brief 不提交新输入地取出尾部，清空未写满的输出后缀。
/// @return 有效尾部帧数；是否彻底结束仍以 is_finished 为准。
/// @warning 音频热路径，只消费已有算法状态，不分配缓冲。
std::size_t RStretcher::drain(AudioBuffer& output)
{
    const std::size_t retrieved = drain_into(output, 0U);
    // 与 process 保持同样的整块输出语义；偏移版本则不替调用方补零。
    if ( retrieved < output.num_frames() ) output.clear_from(retrieved);
    return retrieved;
}

/// @brief 保留输出前缀地提取已有尾部，不自动发送 final 标记。
/// @param output 固定格式输出缓冲。
/// @param outputOffset 追加写入的起始帧，允许恰好位于缓冲末尾。
/// @return 本次实际取出量，未覆盖区域保持原内容。
/// @warning 音频热路径，不能为等待未来输出而在调用者中固定时长阻塞。
std::size_t RStretcher::drain_into(AudioBuffer& output,
                                   std::size_t  outputOffset)
{
    // 提取接口也必须验证格式；它可能在上层没有本次输入时独立调用。
    if ( output.afmt != m_format ||
         output.num_channels() != m_outputPointers.size() ||
         outputOffset > output.num_frames() ) {
        return 0U;
    }

    // 即使输出已满，也查询终止状态；输出容量不足不应伪造结束事件。
    const std::size_t retrieved = retrieveAvailable(output, outputOffset);
    const int         available = m_rubberBandStretcher->available();
    // 不能因本次 retrieved 为零就结束：非 final 流也可能暂时没有产出。
    if ( available < 0 ) m_finished = true;
    return retrieved;
}

/// @brief 查询后端是否已报告彻底读空，而非查询是否已经送过 final。
/// @warning 状态属于处理线程，不能用作其他线程等待处理完成的同步标志。
bool RStretcher::is_finished() const
{
    return m_finished;
}

/// @brief 借用构造时固定的音频格式，引用不延长对象生命周期。
/// @return 与实例同寿命的格式引用；不能用于绕过固定格式限制修改内部状态。
const AudioDataFormat& RStretcher::format() const
{
    return m_format;
}

/// @brief 返回每个新算法流开始前补入的输入静音帧数。
/// @return 构造时查询到的输入侧补偿量，不是待输出帧数。
std::size_t RStretcher::preferred_start_pad() const
{
    return m_preferredStartPad;
}

/// @brief 返回 reset 后必须丢弃的输出延迟总量。
/// @return 输出侧帧数，与输入 padding 不要求相同。
std::size_t RStretcher::start_delay() const
{
    return m_startDelay;
}

/// @brief 查询本段尚未丢弃的起始延迟，可能跨多个处理块逐渐消耗。
/// @warning 这是处理线程的普通状态读，不为 UI 或控制线程提供同步。
std::size_t RStretcher::remaining_start_delay() const
{
    return m_remainingStartDelay;
}

/// @brief 将引擎质量档位翻译为固定的后端选项组合。
/// @return 实时、关联声道、Finer 引擎和禁用内部线程始终启用。
/// @warning 仅构造时使用，不能在音频回调中重新创建算法来应用这些选项。
int RStretcher::makeOptions(TimeStretchQuality quality)
{
    // 声道一起处理，避免独立分析破坏立体声关系。
    // 禁用后端内部线程使调度归属保持在上层，不让回调依赖额外工作线程。
    int options = RubberBand::RubberBandStretcher::OptionProcessRealTime |
                  RubberBand::RubberBandStretcher::OptionChannelsTogether |
                  RubberBand::RubberBandStretcher::OptionEngineFiner |
                  RubberBand::RubberBandStretcher::OptionThreadingNever;

    switch ( quality ) {
    // 档位是窗口长度与音高算法的组合，不是对整个枚举按性能线性插值。
    case TimeStretchQuality::Fast:
        // 短窗与高速音高策略共同用于这个档位。
        options |= RubberBand::RubberBandStretcher::OptionWindowShort |
                   RubberBand::RubberBandStretcher::OptionPitchHighSpeed;
        break;
    case TimeStretchQuality::Balanced:
        // 使用标准窗，但音高仍选择高速策略，避免把它等同于 Best。
        options |= RubberBand::RubberBandStretcher::OptionWindowStandard |
                   RubberBand::RubberBandStretcher::OptionPitchHighSpeed;
        break;
    case TimeStretchQuality::Finer:
        // 短窗保留较短分析窗口，同时选择高一致性的音高策略。
        options |= RubberBand::RubberBandStretcher::OptionWindowShort |
                   RubberBand::RubberBandStretcher::OptionPitchHighConsistency;
        break;
    case TimeStretchQuality::Best:
        // 标准窗与高一致性组合；质量名不意味着内部线程会被启用。
        options |= RubberBand::RubberBandStretcher::OptionWindowStandard |
                   RubberBand::RubberBandStretcher::OptionPitchHighConsistency;
        break;
    }
    return options;
}

/// @brief 用静音预先触达常规输入、短尾和结束刷新的内部工作区。
/// @param maxInputFrames 上层承诺的最大输入工作量。
/// @param maxOutputFrames 一次预热提取的输出容量。
/// @warning 只在构造的控制路径执行；这里包含局部缓冲分配和同步排空循环。
/// 排空依赖已提交 final 的有限算法流，不是等待其他线程或外部输入。
void RStretcher::prewarm(std::size_t maxInputFrames,
                         std::size_t maxOutputFrames)
{
    // 根据时长倍率反推填满输出需要的输入，并给取整边界留一帧余量。
    // 浮点结果先判有限和范围，再转换到 size_t，避免极端倍率转换溢出。
    const double expectedInputFrames =
        std::ceil(static_cast<double>(maxOutputFrames) / m_stretchRatio) + 1.0;
    std::size_t boundedExpectedInputFrames = maxInputFrames;
    if ( std::isfinite(expectedInputFrames) &&
         expectedInputFrames < static_cast<double>(maxInputFrames) ) {
        boundedExpectedInputFrames =
            static_cast<std::size_t>(expectedInputFrames);
    }
    // 输入容量还需满足后端单次处理上限；预热不靠超大临时块触发扩容。
    const std::size_t warmInputFrames = std::max<std::size_t>(
        1U, std::min(boundedExpectedInputFrames, m_maxProcessFrames));
    const std::size_t warmOutputFrames =
        std::max<std::size_t>(1U, maxOutputFrames);

    // 这些缓冲仅存在于构造阶段，不可移动到实时 process 内按次创建。
    AudioBuffer warmInput(m_format, warmInputFrames);
    AudioBuffer warmOutput(m_format, warmOutputFrames);
    warmInput.clear();
    warmOutput.clear();

    // 最短尾段可能走不同的结束处理分支，先用单帧覆盖该路径。
    warmInput.set_active_frames(1U);
    reset();
    process(warmOutput, warmInput, true);
    while ( !is_finished() ) {
        // 已提交有限的 final 输入，仅推进本地算法尾部，不等待新输入到达。
        drain(warmOutput);
    }

    // 再覆盖预期常规块大小的输入与排空，不保留前次的流历史。
    warmInput.set_active_frames(warmInputFrames);
    reset();
    process(warmOutput, warmInput, true);
    while ( !is_finished() ) {
        drain(warmOutput);
    }

    reset();
    // 覆盖非末块到末块的连续状态转换，而不只测试单次 final 输入。
    process(warmOutput, warmInput, false);
    process(warmOutput, warmInput, true);
    while ( !is_finished() ) {
        drain(warmOutput);
    }
    // 丢弃全部预热历史，实例对外仍表现为尚未消费真实输入的新流。
    reset();
}

/// @brief 分块提交静音前缀，使实时算法的首个有效输出与真实输入起点对齐。
/// @warning reset 的热路径子调用，只复用零缓冲，不创建容器。
void RStretcher::primeStart()
{
    if ( m_preferredStartPad == 0U ) return;

    const float* const* paddingChannels = m_paddingInput.raw_ptrs();
    std::size_t         submitted       = 0U;
    while ( submitted < m_preferredStartPad ) {
        // 每次最多读取实际分配的静音容量，最后一块允许不足整块。
        // padding 不是流结束，不能在这里发送 final。
        const std::size_t chunkFrames = std::min(
            m_paddingInput.frame_capacity(), m_preferredStartPad - submitted);
        m_rubberBandStretcher->process(paddingChannels, chunkFrames, false);
        submitted += chunkFrames;
    }
}

/// @brief 先消费算法的起始延迟，避免将填充引入的样本交给调用方。
/// @warning 实时热路径；仅消费当前可用输出，缺数据立即返回而非等待。
void RStretcher::discardStartDelay()
{
    while ( m_remainingStartDelay > 0U ) {
        const int available = m_rubberBandStretcher->available();
        if ( available <= 0 ) {
            // 缺输出时保留剩余延迟，下一输入块继续丢弃；负值另记流终止。
            if ( available < 0 ) m_finished = true;
            return;
        }

        // 同时受可用量、剩余延迟和丢弃工作区限制，不能越界写临时平面。
        const std::size_t framesToDiscard =
            std::min({ static_cast<std::size_t>(available),
                       m_remainingStartDelay,
                       m_delayDiscardOutput.frame_capacity() });
        float** discardChannels = m_delayDiscardOutput.raw_ptrs();
        for ( std::uint16_t channel = 0U; channel < m_format.channels;
              ++channel ) {
            m_outputPointers[channel] = discardChannels[channel];
        }

        const std::size_t discarded = m_rubberBandStretcher->retrieve(
            m_outputPointers.data(), framesToDiscard);
        // 后端本轮无进展时退出，避免在音频线程围绕零产出忙等。
        if ( discarded == 0U ) return;
        m_remainingStartDelay -= discarded;
        // 跨块补偿量以实际丢弃数递减，不能把请求数当作已成功消费数。
    }
}

/// @brief 补偿起始延迟后，把可用样本拷入输出的剩余区间。
/// @param output 由上层验证过格式与声道数量的缓冲。
/// @param outputOffset 不大于活动输出帧数的起点。
/// @return 实际写入帧数；不包括被丢弃的启动延迟。
/// @warning 实时热路径，复用声道指针数组；遇到零产出或无可用量立即结束。
std::size_t RStretcher::retrieveAvailable(AudioBuffer& output,
                                          std::size_t  outputOffset)
{
    // 补偿未完成之前，任何后端输出都不能进入调用方的有效音频区间。
    discardStartDelay();
    if ( m_remainingStartDelay > 0U ) return 0U;

    std::size_t totalRetrieved = 0U;
    while ( outputOffset + totalRetrieved < output.num_frames() ) {
        const int available = m_rubberBandStretcher->available();
        if ( available <= 0 ) break;

        // available 可以大于调用者剩余空间，尾部留在后端供下次 drain。
        const std::size_t remaining =
            output.num_frames() - outputOffset - totalRetrieved;
        const std::size_t framesToRetrieve =
            std::min(static_cast<std::size_t>(available), remaining);
        // 偏移作用于每个平面，不乘声道数；此前已写入的前缀保持不变。
        float** outputChannels = output.raw_ptrs();
        for ( std::uint16_t channel = 0U; channel < m_format.channels;
              ++channel ) {
            m_outputPointers[channel] =
                outputChannels[channel] + outputOffset + totalRetrieved;
        }

        const std::size_t actual = m_rubberBandStretcher->retrieve(
            m_outputPointers.data(), framesToRetrieve);
        // 以实际取出量推进，不能假定后端一定返回请求量。
        if ( actual == 0U ) break;
        totalRetrieved += actual;
    }
    return totalRetrieved;
}

}  // namespace ice
