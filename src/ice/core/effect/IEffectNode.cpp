#include "ice/core/effect/IEffectNode.hpp"

#include "ice/config/config.hpp"

#include <algorithm>

namespace ice
{
/// @brief 在节点发布到音频图之前准备默认上游缓冲。
/// @warning 构造可能分配，不得在实时拉取链中创建节点。
IEffectNode::IEffectNode()
{
    // 默认长度至少一帧，避免全局配置为零时得到不可用的默认准备状态。
    prepare(ICEConfig::internal_format,
            std::max<std::size_t>(ICEConfig::default_buffer_size, 1U));
}

/// @brief 记录固定格式和容量上限，一次性准备后续复用空间。
/// @warning 仅允许处理停止时调用；可能扩容且不与 process 并发安全。
void IEffectNode::prepare(const AudioDataFormat& format, std::size_t maxFrames)
{
    // 准备参数和缓冲作为一个控制侧状态更新，运行后保持稳定。
    m_preparedFormat    = format;
    m_maxPreparedFrames = maxFrames;
    m_isPrepared =
        format.channels > 0U && format.samplerate > 0U && maxFrames > 0U;

    // 无效参数使节点进入拒绝处理状态，而不是在回调中自动猜测格式。
    if ( m_isPrepared ) {
        // 参数有效不代表存储已准备；失败时禁止后续短块沿用旧缓冲拉取上游。
        m_isPrepared = inputBuffer.resize(format, maxFrames);
        // 清除新准备的空间，防止第一次拉取包含未初始化或旧音频。
        if ( m_isPrepared ) inputBuffer.clear();
    } else {
        // 无效准备仅保留零有效长度，不向上游请求数据。
        inputBuffer.resize(format, 0U);
    }
}

/// @brief 验证输出块后拉取上游，再让派生效果写入输出。
/// @warning 每音频块调用，禁止加锁、分配或增加上游共享引用计数。
void IEffectNode::process(AudioBuffer& buffer)
{
    // 上游所有权由节点成员维持，处理期间禁止控制侧替换它。
    IAudioNode* const input = inputNode.get();
    // 只修改预分配空间的有效长度，超出上限时静音拒绝而非现场扩容。
    if ( !input || !m_isPrepared || buffer.afmt != m_preparedFormat ||
         buffer.num_frames() > m_maxPreparedFrames ||
         !inputBuffer.set_active_frames(buffer.num_frames()) ) {
        // 无上游、未准备及容量不足都以静音返回，不在此重试或修复音频图。
        buffer.clear();
        return;
    }

    // 上游允许短读或提前返回，预先清零保证未写入部分为静音。
    inputBuffer.clear();
    input->process(inputBuffer);
    // 上游不得擅自改变格式或块长，否则派生算法不能按预期索引缓冲。
    if ( inputBuffer.afmt != m_preparedFormat ||
         inputBuffer.num_frames() != buffer.num_frames() ) {
        buffer.clear();
        return;
    }

    // 输出和输入是独立缓冲；派生类负责写出本块完整结果。
    // 基类不复制输入到输出，空实现不会自动成为直通效果。
    apply_effect(buffer, inputBuffer);
}
}  // namespace ice
