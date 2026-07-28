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
    if ( !std::isfinite(ratio) || ratio <= 0.0 ) return 1.0;
    return ratio;
}

}  // namespace

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
    m_rubberBandStretcher =
        std::make_unique<RubberBand::RubberBandStretcher>(format.samplerate,
                                                          format.channels,
                                                          makeOptions(quality),
                                                          m_stretchRatio,
                                                          m_pitchRatio);

    m_preferredStartPad = m_rubberBandStretcher->getPreferredStartPad();
    m_startDelay        = m_rubberBandStretcher->getStartDelay();

    const std::size_t processLimit =
        m_rubberBandStretcher->getProcessSizeLimit();
    const std::size_t reservedProcessFrames =
        std::max({ m_maxInputFrames,
                   m_maxOutputFrames,
                   std::max<std::size_t>(1U, m_preferredStartPad) });
    m_maxProcessFrames = std::max<std::size_t>(
        1U, std::min(reservedProcessFrames, processLimit));
    m_rubberBandStretcher->setMaxProcessSize(m_maxProcessFrames);

    m_paddingInput.resize(
        m_format,
        std::max<std::size_t>(
            1U, std::min(m_preferredStartPad, m_maxProcessFrames)));
    m_delayDiscardOutput.resize(m_format, m_maxOutputFrames);
    m_paddingInput.clear();
    m_delayDiscardOutput.clear();

    prewarm(m_maxInputFrames, m_maxOutputFrames);
}

RStretcher::~RStretcher() = default;

void RStretcher::set_stretch_ratio(double ratio)
{
    const double sanitized = sanitizeRatio(ratio);
    if ( std::abs(sanitized - m_stretchRatio) <=
         std::numeric_limits<double>::epsilon() ) {
        return;
    }
    m_stretchRatio = sanitized;
    m_rubberBandStretcher->setTimeRatio(sanitized);
}

void RStretcher::set_pitch_ratio(double pitchRatio)
{
    const double sanitized = sanitizeRatio(pitchRatio);
    if ( std::abs(sanitized - m_pitchRatio) <=
         std::numeric_limits<double>::epsilon() ) {
        return;
    }
    m_pitchRatio = sanitized;
    m_rubberBandStretcher->setPitchScale(sanitized);
}

double RStretcher::get_stretch_ratio() const
{
    return m_stretchRatio;
}

double RStretcher::get_pitch_ratio() const
{
    return m_pitchRatio;
}

void RStretcher::reset()
{
    m_rubberBandStretcher->reset();
    m_finished            = false;
    m_remainingStartDelay = m_startDelay;
    primeStart();
}

std::size_t RStretcher::process(AudioBuffer& output, const AudioBuffer& input,
                                bool finalInput)
{
    const std::size_t written = process_into(output, 0U, input, finalInput);
    if ( written < output.num_frames() ) output.clear_from(written);
    return written;
}

std::size_t RStretcher::process_into(AudioBuffer&       output,
                                     std::size_t        outputOffset,
                                     const AudioBuffer& input, bool finalInput)
{
    if ( output.afmt != m_format || input.afmt != m_format ||
         input.num_channels() != m_inputPointers.size() ||
         output.num_channels() != m_outputPointers.size() ||
         outputOffset > output.num_frames() || m_finished ) {
        return 0U;
    }

    const float* const* inputChannels = input.raw_ptrs();
    const std::size_t   inputFrames   = input.num_frames();
    std::size_t         inputOffset   = 0U;
    std::size_t         writtenFrames = 0U;

    while ( inputOffset < inputFrames ) {
        const std::size_t chunkFrames =
            std::min(m_maxProcessFrames, inputFrames - inputOffset);
        for ( std::uint16_t channel = 0U; channel < m_format.channels;
              ++channel ) {
            m_inputPointers[channel] = inputChannels[channel] + inputOffset;
        }

        const bool isFinalChunk =
            finalInput && inputOffset + chunkFrames == inputFrames;
        m_rubberBandStretcher->process(
            m_inputPointers.data(), chunkFrames, isFinalChunk);
        inputOffset += chunkFrames;

        if ( outputOffset + writtenFrames < output.num_frames() ) {
            writtenFrames +=
                retrieveAvailable(output, outputOffset + writtenFrames);
        }
    }

    if ( inputFrames == 0U && finalInput ) {
        m_rubberBandStretcher->process(m_inputPointers.data(), 0U, true);
    }
    if ( outputOffset + writtenFrames < output.num_frames() ) {
        writtenFrames +=
            retrieveAvailable(output, outputOffset + writtenFrames);
    }

    const int available = m_rubberBandStretcher->available();
    if ( available < 0 ) m_finished = true;
    return writtenFrames;
}

std::size_t RStretcher::drain(AudioBuffer& output)
{
    const std::size_t retrieved = drain_into(output, 0U);
    if ( retrieved < output.num_frames() ) output.clear_from(retrieved);
    return retrieved;
}

std::size_t RStretcher::drain_into(AudioBuffer& output,
                                   std::size_t  outputOffset)
{
    if ( output.afmt != m_format ||
         output.num_channels() != m_outputPointers.size() ||
         outputOffset > output.num_frames() ) {
        return 0U;
    }

    const std::size_t retrieved = retrieveAvailable(output, outputOffset);
    const int         available = m_rubberBandStretcher->available();
    if ( available < 0 ) m_finished = true;
    return retrieved;
}

bool RStretcher::is_finished() const
{
    return m_finished;
}

const AudioDataFormat& RStretcher::format() const
{
    return m_format;
}

std::size_t RStretcher::preferred_start_pad() const
{
    return m_preferredStartPad;
}

std::size_t RStretcher::start_delay() const
{
    return m_startDelay;
}

std::size_t RStretcher::remaining_start_delay() const
{
    return m_remainingStartDelay;
}

int RStretcher::makeOptions(TimeStretchQuality quality)
{
    int options = RubberBand::RubberBandStretcher::OptionProcessRealTime |
                  RubberBand::RubberBandStretcher::OptionChannelsTogether |
                  RubberBand::RubberBandStretcher::OptionEngineFiner |
                  RubberBand::RubberBandStretcher::OptionThreadingNever;

    switch ( quality ) {
    case TimeStretchQuality::Fast:
        options |= RubberBand::RubberBandStretcher::OptionWindowShort |
                   RubberBand::RubberBandStretcher::OptionPitchHighSpeed;
        break;
    case TimeStretchQuality::Balanced:
        options |= RubberBand::RubberBandStretcher::OptionWindowStandard |
                   RubberBand::RubberBandStretcher::OptionPitchHighSpeed;
        break;
    case TimeStretchQuality::Finer:
        options |= RubberBand::RubberBandStretcher::OptionWindowShort |
                   RubberBand::RubberBandStretcher::OptionPitchHighConsistency;
        break;
    case TimeStretchQuality::Best:
        options |= RubberBand::RubberBandStretcher::OptionWindowStandard |
                   RubberBand::RubberBandStretcher::OptionPitchHighConsistency;
        break;
    }
    return options;
}

void RStretcher::prewarm(std::size_t maxInputFrames,
                         std::size_t maxOutputFrames)
{
    const double expectedInputFrames =
        std::ceil(static_cast<double>(maxOutputFrames) / m_stretchRatio) + 1.0;
    std::size_t boundedExpectedInputFrames = maxInputFrames;
    if ( std::isfinite(expectedInputFrames) &&
         expectedInputFrames < static_cast<double>(maxInputFrames) ) {
        boundedExpectedInputFrames =
            static_cast<std::size_t>(expectedInputFrames);
    }
    const std::size_t warmInputFrames = std::max<std::size_t>(
        1U, std::min(boundedExpectedInputFrames, m_maxProcessFrames));
    const std::size_t warmOutputFrames =
        std::max<std::size_t>(1U, maxOutputFrames);

    AudioBuffer warmInput(m_format, warmInputFrames);
    AudioBuffer warmOutput(m_format, warmOutputFrames);
    warmInput.clear();
    warmOutput.clear();

    warmInput.set_active_frames(1U);
    reset();
    process(warmOutput, warmInput, true);
    while ( !is_finished() ) {
        drain(warmOutput);
    }

    warmInput.set_active_frames(warmInputFrames);
    reset();
    process(warmOutput, warmInput, true);
    while ( !is_finished() ) {
        drain(warmOutput);
    }

    reset();
    process(warmOutput, warmInput, false);
    process(warmOutput, warmInput, true);
    while ( !is_finished() ) {
        drain(warmOutput);
    }
    reset();
}

void RStretcher::primeStart()
{
    if ( m_preferredStartPad == 0U ) return;

    const float* const* paddingChannels = m_paddingInput.raw_ptrs();
    std::size_t         submitted       = 0U;
    while ( submitted < m_preferredStartPad ) {
        const std::size_t chunkFrames = std::min(
            m_paddingInput.frame_capacity(), m_preferredStartPad - submitted);
        m_rubberBandStretcher->process(paddingChannels, chunkFrames, false);
        submitted += chunkFrames;
    }
}

void RStretcher::discardStartDelay()
{
    while ( m_remainingStartDelay > 0U ) {
        const int available = m_rubberBandStretcher->available();
        if ( available <= 0 ) {
            if ( available < 0 ) m_finished = true;
            return;
        }

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
        if ( discarded == 0U ) return;
        m_remainingStartDelay -= discarded;
    }
}

std::size_t RStretcher::retrieveAvailable(AudioBuffer& output,
                                          std::size_t  outputOffset)
{
    discardStartDelay();
    if ( m_remainingStartDelay > 0U ) return 0U;

    std::size_t totalRetrieved = 0U;
    while ( outputOffset + totalRetrieved < output.num_frames() ) {
        const int available = m_rubberBandStretcher->available();
        if ( available <= 0 ) break;

        const std::size_t remaining =
            output.num_frames() - outputOffset - totalRetrieved;
        const std::size_t framesToRetrieve =
            std::min(static_cast<std::size_t>(available), remaining);
        float** outputChannels = output.raw_ptrs();
        for ( std::uint16_t channel = 0U; channel < m_format.channels;
              ++channel ) {
            m_outputPointers[channel] =
                outputChannels[channel] + outputOffset + totalRetrieved;
        }

        const std::size_t actual = m_rubberBandStretcher->retrieve(
            m_outputPointers.data(), framesToRetrieve);
        if ( actual == 0U ) break;
        totalRetrieved += actual;
    }
    return totalRetrieved;
}

}  // namespace ice
