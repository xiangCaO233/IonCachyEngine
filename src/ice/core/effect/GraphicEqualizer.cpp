#include "ice/core/effect/GraphicEqualizer.hpp"

#include "ice/config/config.hpp"
#include "ice/core/effect/filter/BiquadFilter.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

namespace ice
{
struct GraphicEqualizer::PreparedFilterState {
    /// @brief 状态适用的固定音频格式。
    AudioDataFormat format{};

    /// @brief 每个声道独立的滤波器链及其历史值。
    std::vector<std::vector<BiquadFilter>> filterChains;
};

GraphicEqualizer::GraphicEqualizer(const std::vector<double>& centerFrequencies)
{
    static_assert(std::atomic<PreparedFilterState*>::is_always_lock_free,
                  "GraphicEqualizer 的音频线程状态指针必须为无锁原子");

    m_bands.reserve(centerFrequencies.size());
    for ( const double frequency : centerFrequencies ) {
        m_bands.push_back(EQBandOptions{ .center_freq_hz = frequency });
    }

    prepare(ICEConfig::internal_format,
            std::max<std::size_t>(ICEConfig::default_buffer_size, 1U));
}

GraphicEqualizer::~GraphicEqualizer() = default;

void GraphicEqualizer::prepare(const AudioDataFormat& format,
                               std::size_t            maxFrames)
{
    IEffectNode::prepare(format, maxFrames);

    std::lock_guard<std::mutex> lock(m_controlMutex);
    m_preparedFormat = format;
    publish_filter_state_locked();
}

void GraphicEqualizer::set_band_gain_ratio(std::size_t bandIndex, float ratio)
{
    if ( ratio <= 0.0001F ) return;

    std::lock_guard<std::mutex> lock(m_controlMutex);
    if ( bandIndex >= m_bands.size() ) return;

    m_bands[bandIndex].gain_db = 20.0 * std::log10(static_cast<double>(ratio));
    publish_filter_state_locked();
}

void GraphicEqualizer::set_band_gain_db(std::size_t bandIndex, float db)
{
    std::lock_guard<std::mutex> lock(m_controlMutex);
    if ( bandIndex >= m_bands.size() ) return;

    m_bands[bandIndex].gain_db = db;
    publish_filter_state_locked();
}

void GraphicEqualizer::set_band_q_factor(std::size_t bandIndex, float q)
{
    if ( q <= 0.0F ) return;

    std::lock_guard<std::mutex> lock(m_controlMutex);
    if ( bandIndex >= m_bands.size() ) return;

    m_bands[bandIndex].q_factor = q;
    publish_filter_state_locked();
}

std::size_t GraphicEqualizer::get_band_count() const
{
    std::lock_guard<std::mutex> lock(m_controlMutex);
    return m_bands.size();
}

double GraphicEqualizer::get_band_frequency(std::size_t bandIndex) const
{
    std::lock_guard<std::mutex> lock(m_controlMutex);
    return bandIndex < m_bands.size() ? m_bands[bandIndex].center_freq_hz : 0.0;
}

double GraphicEqualizer::get_band_gain_db(std::size_t bandIndex) const
{
    std::lock_guard<std::mutex> lock(m_controlMutex);
    return bandIndex < m_bands.size() ? m_bands[bandIndex].gain_db : 0.0;
}

double GraphicEqualizer::get_band_q_factor(std::size_t bandIndex) const
{
    std::lock_guard<std::mutex> lock(m_controlMutex);
    return bandIndex < m_bands.size() ? m_bands[bandIndex].q_factor : 1.0;
}

double GraphicEqualizer::get_total_magnitude_response(double frequency) const
{
    std::lock_guard<std::mutex> lock(m_controlMutex);
    if ( m_preparedFormat.samplerate == 0U ) return 1.0;

    double totalMagnitude = 1.0;
    for ( const EQBandOptions& band : m_bands ) {
        BiquadFilter responseFilter;
        responseFilter.set_peaking(
            static_cast<double>(m_preparedFormat.samplerate),
            band.center_freq_hz,
            band.q_factor,
            band.gain_db);
        totalMagnitude *= responseFilter.get_magnitude_response(
            frequency, static_cast<double>(m_preparedFormat.samplerate));
    }
    return totalMagnitude;
}

void GraphicEqualizer::reclaim_retired_filter_states()
{
    std::lock_guard<std::mutex> lock(m_controlMutex);
    reclaim_retired_filter_states_locked();
}

std::size_t GraphicEqualizer::retired_filter_state_count() const
{
    std::lock_guard<std::mutex> lock(m_controlMutex);
    return m_retiredStates.size();
}

void GraphicEqualizer::apply_effect(AudioBuffer&       output,
                                    const AudioBuffer& input)
{
    if ( output.afmt != input.afmt ||
         output.num_frames() != input.num_frames() ) {
        output.clear();
        return;
    }

    float**             outputSamples = output.raw_ptrs();
    const float* const* inputSamples  = input.raw_ptrs();
    if ( !outputSamples || !inputSamples ) {
        output.clear();
        return;
    }

    for ( std::uint16_t channel = 0U; channel < input.num_channels();
          ++channel ) {
        std::memcpy(outputSamples[channel],
                    inputSamples[channel],
                    input.num_frames() * sizeof(float));
    }

    PreparedFilterState* const state = acquire_filter_state();
    if ( !state ) return;

    if ( state->format != output.afmt ||
         state->filterChains.size() != output.num_channels() ) {
        release_filter_state();
        return;
    }

    for ( std::uint16_t channel = 0U; channel < output.num_channels();
          ++channel ) {
        float* channelSamples = outputSamples[channel];
        for ( BiquadFilter& filter : state->filterChains[channel] ) {
            filter.process(channelSamples, output.num_frames());
        }
    }
    release_filter_state();
}

void GraphicEqualizer::publish_filter_state_locked()
{
    auto nextState    = std::make_unique<PreparedFilterState>();
    nextState->format = m_preparedFormat;
    nextState->filterChains.resize(m_preparedFormat.channels);

    for ( auto& chain : nextState->filterChains ) {
        chain.resize(m_bands.size());
        for ( std::size_t bandIndex = 0U; bandIndex < m_bands.size();
              ++bandIndex ) {
            const EQBandOptions& band = m_bands[bandIndex];
            chain[bandIndex].set_peaking(
                static_cast<double>(m_preparedFormat.samplerate),
                band.center_freq_hz,
                band.q_factor,
                band.gain_db);
        }
    }

    PreparedFilterState* const nextAddress = nextState.get();
    if ( m_activeStateOwner ) {
        m_retiredStates.push_back(std::move(m_activeStateOwner));
    }
    m_activeStateOwner = std::move(nextState);
    m_activeState.store(nextAddress, std::memory_order_seq_cst);
    reclaim_retired_filter_states_locked();
}

void GraphicEqualizer::reclaim_retired_filter_states_locked()
{
    PreparedFilterState* const protectedState =
        m_hazardState.load(std::memory_order_seq_cst);
    std::erase_if(
        m_retiredStates,
        [protectedState](const std::unique_ptr<PreparedFilterState>& state) {
            return state.get() != protectedState;
        });
}

GraphicEqualizer::PreparedFilterState*
GraphicEqualizer::acquire_filter_state() noexcept
{
    PreparedFilterState* state{ nullptr };
    do {
        state = m_activeState.load(std::memory_order_seq_cst);
        m_hazardState.store(state, std::memory_order_seq_cst);
    } while ( state != m_activeState.load(std::memory_order_seq_cst) );
    return state;
}

void GraphicEqualizer::release_filter_state() noexcept
{
    m_hazardState.store(nullptr, std::memory_order_seq_cst);
}

}  // namespace ice
