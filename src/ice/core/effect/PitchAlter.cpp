#include <ice/core/effect/PitchAlter.hpp>

#include <ice/core/effect/rubberband/RStretcher.hpp>
#include <ice/manage/AudioBuffer.hpp>

#include <cmath>
#include <cstdint>
#include <memory>

namespace ice
{
namespace
{
/// @brief 单位倍率附近保留历史直通阈值。
constexpr double UNITY_PITCH_TOLERANCE = 0.001F;

/// @brief 确认上游处理后仍提供有效的声道平面。
/// @warning 每音频块最多调用一次，只遍历声道指针，不分配。
bool hasReadablePlanes(const AudioBuffer& input)
{
    // 空块不会读取 PCM，允许没有平面表。
    if ( input.num_frames() == 0 ) return true;
    // 帧容量和声道地址可能由自定义上游更换，分别验证。
    const auto planes = input.raw_ptrs();
    if ( !planes || input.frame_capacity() < input.num_frames() ) return false;
    // 自定义上游可能更换平面地址，不能将空声道交给累加或变调算法。
    for ( std::uint16_t channel = 0; channel < input.num_channels();
          ++channel ) {
        if ( !planes[channel] ) return false;
    }
    return true;
}
}  // namespace

/// @brief 继承基类默认准备状态，保持默认构造后的直通用法。
PitchAlter::PitchAlter()
{
    // 基类构造期间不会分派到派生准备入口，派生构造完成后记录默认配置。
    // 默认倍率直通，只记录最大逻辑块，不创建不需要的后端。
    prepare_effect(get_inputbuffer().afmt);
}

/// @brief 接受有限正倍率，保存等价的半音数。
void PitchAlter::set_pitch_scale(double scale)
{
    if ( !std::isfinite(scale) || scale <= 0.0 ) return;
    // 对数换算在控制侧执行，音频回调只消费准备好的结果。
    updatePitch(scale, 12.0 * std::log2(scale));
}

/// @brief 接受可表达为有限正倍率的半音数。
void PitchAlter::set_pitch_shift(double semitones)
{
    // 半音接口保留输入的精确表示，而后端只需要换算后的倍率。
    const double scale = std::exp2(semitones / 12.0);
    // 拒绝指数溢出与下溢，避免把无效配置发布给新后端。
    if ( !std::isfinite(scale) || scale <= 0.0 ) return;
    updatePitch(scale, semitones);
}

/// @brief 在停止处理时提交同一组参数。
void PitchAlter::updatePitch(double scale, double semitones)
{
    // 更新半音表示不要求重建输出结果完全相同的配置。
    m_pitchSemitones = semitones;
    // 设置等价倍率时无需重新预热，也不会清除已准备的历史。
    if ( m_pitchScale == scale ) return;
    // 先完成整个控制侧配置，再构造会随配置发布给回调的后端。
    m_pitchScale = scale;
    prepareStretcher();
}

/// @brief 读取由基类刚准备的满容量逻辑块。
void PitchAlter::prepare_effect(const AudioDataFormat& format)
{
    // 重新准备即使倍率不变，也不能沿用与旧声道布局关联的历史。
    m_preparedFormat = format;
    // 此钩子只在控制侧进入，prepare 刚把逻辑帧数设为声明上限。
    // 不能从回调后的短块读取此值，也不能把对齐分配的额外帧数算作上限。
    m_preparedFrames = get_inputbuffer().num_frames();
    prepareStretcher();
}

/// @brief 建立不需要在处理过程中调整倍率的固定后端。
void PitchAlter::prepareStretcher()
{
    // 格式无效时不能创建算法；单位倍率也无需保留算法状态。
    if ( m_preparedFrames == 0 || m_preparedFormat.channels == 0 ||
         m_preparedFormat.samplerate == 0 ||
         std::abs(m_pitchScale - 1.0) < UNITY_PITCH_TOLERANCE ) {
        // 直通与无效准备都不需要算法历史；后端只在控制侧释放。
        m_stretcher.reset();
        return;
    }
    // 预热与首次填充逻辑都不留到音频设备回调执行。
    // 构造传入最终倍率与最大块长，避免回调内调用可重新分配的倍率 setter。
    // 时间倍率固定为一，输入和输出预热长度来自同一个设备块约定。
    m_stretcher = std::make_unique<RStretcher>(m_preparedFormat,
                                               TimeStretchQuality::Finer,
                                               m_preparedFrames,
                                               m_preparedFrames,
                                               1.0,
                                               m_pitchScale);
}

/// @brief 借用稳定上游，保持节点作为可选混音支路时的空输入语义。
/// @warning 音频热路径，不读取控制参数或申请缓冲。
void PitchAlter::process(AudioBuffer& buffer)
{
    // 未连接的效果不改写已有混音样本；连接后统一由基类拒绝不合规的块。
    if ( !get_inputnode_observer() ) return;
    IEffectNode::process(buffer);
}

/// @brief 消费基类验证并拉取的块，保持原有输出语义。
/// @warning 音频热路径，每块只读取稳定后端，禁止分配及参数重建。
void PitchAlter::apply_effect(AudioBuffer& output, const AudioBuffer& input)
{
    // 基类检查格式和逻辑长度，这里额外验证上游填充后的物理存储。
    if ( !hasReadablePlanes(input) ) {
        // 拒绝异常上游时不能把不完整的 PCM 推进变调历史。
        output.clear();
        return;
    }
    // 控制侧同时确定分支和倍率，处理侧无需再读取参数。
    if ( m_stretcher ) {
        // 后端的启动延迟可能产生短输出，未写入部分由包装器清零。
        m_stretcher->process(output, input);
    } else {
        // 直通沿用历史累加语义，保留调用者已经放在输出中的样本。
        output += input;
    }
}
}  // namespace ice
