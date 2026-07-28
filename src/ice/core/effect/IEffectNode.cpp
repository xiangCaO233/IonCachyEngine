#include "ice/core/effect/IEffectNode.hpp"

#include "ice/config/config.hpp"

#include <algorithm>

namespace ice
{
IEffectNode::IEffectNode()
{
    prepare(ICEConfig::internal_format,
            std::max<std::size_t>(ICEConfig::default_buffer_size, 1U));
}

void IEffectNode::prepare(const AudioDataFormat& format, std::size_t maxFrames)
{
    m_preparedFormat    = format;
    m_maxPreparedFrames = maxFrames;
    m_isPrepared =
        format.channels > 0U && format.samplerate > 0U && maxFrames > 0U;

    if ( m_isPrepared ) {
        inputBuffer.resize(format, maxFrames);
        inputBuffer.clear();
    } else {
        inputBuffer.resize(format, 0U);
    }
}

void IEffectNode::process(AudioBuffer& buffer)
{
    IAudioNode* const input = inputNode.get();
    if ( !input || !m_isPrepared || buffer.afmt != m_preparedFormat ||
         buffer.num_frames() > m_maxPreparedFrames ||
         !inputBuffer.set_active_frames(buffer.num_frames()) ) {
        buffer.clear();
        return;
    }

    inputBuffer.clear();
    input->process(inputBuffer);
    if ( inputBuffer.afmt != m_preparedFormat ||
         inputBuffer.num_frames() != buffer.num_frames() ) {
        buffer.clear();
        return;
    }

    apply_effect(buffer, inputBuffer);
}
}  // namespace ice
