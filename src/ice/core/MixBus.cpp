#include "ice/core/MixBus.hpp"

#include "ice/config/config.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

namespace ice
{
struct MixBus::SourceSnapshot {
    /// @brief 按控制线程插入顺序固定的来源列表。
    std::vector<std::shared_ptr<IAudioNode>> sources;
};

MixBus::MixBus()
{
    auto initialSnapshot = std::make_unique<SourceSnapshot>();
    m_activeSnapshot.store(initialSnapshot.get(), std::memory_order_seq_cst);
    m_activeSnapshotOwner = std::move(initialSnapshot);

    prepare(ICEConfig::internal_format,
            std::max<std::size_t>(ICEConfig::default_buffer_size, 1U));
}

MixBus::~MixBus() = default;

void MixBus::prepare(const AudioDataFormat& format, std::size_t maxFrames)
{
    std::lock_guard<std::mutex> lock(m_controlMutex);
    m_preparedFormat    = format;
    m_maxPreparedFrames = maxFrames;
    m_isPrepared =
        format.channels > 0U && format.samplerate > 0U && maxFrames > 0U;

    if ( m_isPrepared ) {
        m_tempBuffer.resize(format, maxFrames);
        m_tempBuffer.clear();
    } else {
        m_tempBuffer.resize(format, 0U);
    }
}

void MixBus::process(AudioBuffer& buffer)
{
    buffer.clear();

    const std::size_t frameCount = buffer.num_frames();
    if ( frameCount == 0U ) {
        finalizeOutput(buffer);
        return;
    }

    if ( !m_isPrepared || buffer.afmt != m_preparedFormat ||
         m_maxPreparedFrames == 0U ) {
        m_rejectedProcessCount.fetch_add(1U, std::memory_order_relaxed);
        finalizeOutput(buffer);
        return;
    }

    if ( frameCount > m_maxPreparedFrames ) {
        m_oversizedProcessCount.fetch_add(1U, std::memory_order_relaxed);
    }

    const SourceSnapshot* snapshot = acquireSourceSnapshot();
    std::size_t           outputOffset{ 0U };
    while ( outputOffset < frameCount ) {
        const std::size_t chunkFrames =
            std::min(m_maxPreparedFrames, frameCount - outputOffset);

        for ( const auto& source : snapshot->sources ) {
            if ( !m_tempBuffer.set_active_frames(chunkFrames) ) {
                releaseSourceSnapshot();
                buffer.clear();
                m_rejectedProcessCount.fetch_add(1U, std::memory_order_relaxed);
                finalizeOutput(buffer);
                return;
            }
            m_tempBuffer.clear();
            source->process(m_tempBuffer);

            if ( m_tempBuffer.afmt != m_preparedFormat ||
                 m_tempBuffer.num_frames() != chunkFrames ) {
                releaseSourceSnapshot();
                buffer.clear();
                m_rejectedProcessCount.fetch_add(1U, std::memory_order_relaxed);
                finalizeOutput(buffer);
                return;
            }

            float**             output = buffer.raw_ptrs();
            const float* const* input  = m_tempBuffer.raw_ptrs();
            if ( !output || !input ) {
                releaseSourceSnapshot();
                buffer.clear();
                m_rejectedProcessCount.fetch_add(1U, std::memory_order_relaxed);
                finalizeOutput(buffer);
                return;
            }

            for ( std::uint16_t channel = 0U;
                  channel < m_preparedFormat.channels;
                  ++channel ) {
                float*       destination = output[channel] + outputOffset;
                const float* sourceData  = input[channel];
                for ( std::size_t frame = 0U; frame < chunkFrames; ++frame ) {
                    destination[frame] += sourceData[frame];
                }
            }
        }

        outputOffset += chunkFrames;
    }
    releaseSourceSnapshot();

    finalizeOutput(buffer);
}

void MixBus::add_source(std::shared_ptr<IAudioNode> src)
{
    if ( !src ) return;

    std::lock_guard<std::mutex> lock(m_controlMutex);
    const auto                  existing =
        std::find(m_controlSources.begin(), m_controlSources.end(), src);
    if ( existing != m_controlSources.end() ) return;

    m_controlSources.push_back(std::move(src));
    publishSourceSnapshotLocked();
}

void MixBus::remove_source(const std::shared_ptr<IAudioNode>& src)
{
    if ( !src ) return;

    std::lock_guard<std::mutex> lock(m_controlMutex);
    const auto                  existing =
        std::find(m_controlSources.begin(), m_controlSources.end(), src);
    if ( existing == m_controlSources.end() ) return;

    m_controlSources.erase(existing);
    publishSourceSnapshotLocked();
}

bool MixBus::replace_source(const std::shared_ptr<IAudioNode>& current,
                            std::shared_ptr<IAudioNode>        replacement)
{
    if ( !current || !replacement ) return false;

    std::lock_guard<std::mutex> lock(m_controlMutex);
    const auto                  currentPosition =
        std::find(m_controlSources.begin(), m_controlSources.end(), current);
    if ( currentPosition == m_controlSources.end() ) return false;
    if ( current == replacement ) return true;

    const auto replacementPosition = std::find(
        m_controlSources.begin(), m_controlSources.end(), replacement);
    if ( replacementPosition != m_controlSources.end() ) return false;

    *currentPosition = std::move(replacement);
    publishSourceSnapshotLocked();
    return true;
}

void MixBus::clear()
{
    std::lock_guard<std::mutex> lock(m_controlMutex);
    if ( m_controlSources.empty() ) {
        reclaimRetiredSourcesLocked();
        return;
    }

    m_controlSources.clear();
    publishSourceSnapshotLocked();
}

void MixBus::reclaimRetiredSources()
{
    std::lock_guard<std::mutex> lock(m_controlMutex);
    reclaimRetiredSourcesLocked();
}

std::size_t MixBus::sourceCount() const
{
    std::lock_guard<std::mutex> lock(m_controlMutex);
    return m_controlSources.size();
}

std::size_t MixBus::retiredSnapshotCount() const
{
    std::lock_guard<std::mutex> lock(m_controlMutex);
    return m_retiredSnapshots.size();
}

std::size_t MixBus::maxPreparedFrames() const
{
    return m_isPrepared ? m_maxPreparedFrames : 0U;
}

std::uint64_t MixBus::oversizedProcessCount() const
{
    return m_oversizedProcessCount.load(std::memory_order_relaxed);
}

std::uint64_t MixBus::rejectedProcessCount() const
{
    return m_rejectedProcessCount.load(std::memory_order_relaxed);
}

void MixBus::publishSourceSnapshotLocked()
{
    auto nextSnapshot                 = std::make_unique<SourceSnapshot>();
    nextSnapshot->sources             = m_controlSources;
    const SourceSnapshot* nextAddress = nextSnapshot.get();

    if ( m_activeSnapshotOwner ) {
        m_retiredSnapshots.push_back(std::move(m_activeSnapshotOwner));
    }
    m_activeSnapshotOwner = std::move(nextSnapshot);
    m_activeSnapshot.store(nextAddress, std::memory_order_seq_cst);
    reclaimRetiredSourcesLocked();
}

void MixBus::reclaimRetiredSourcesLocked()
{
    const SourceSnapshot* protectedSnapshot =
        m_hazardSnapshot.load(std::memory_order_seq_cst);
    std::erase_if(
        m_retiredSnapshots,
        [protectedSnapshot](const std::unique_ptr<SourceSnapshot>& snapshot) {
            return snapshot.get() != protectedSnapshot;
        });
}

const MixBus::SourceSnapshot* MixBus::acquireSourceSnapshot() noexcept
{
    const SourceSnapshot* snapshot{ nullptr };
    do {
        snapshot = m_activeSnapshot.load(std::memory_order_seq_cst);
        m_hazardSnapshot.store(snapshot, std::memory_order_seq_cst);
    } while ( snapshot != m_activeSnapshot.load(std::memory_order_seq_cst) );
    return snapshot;
}

void MixBus::releaseSourceSnapshot() noexcept
{
    m_hazardSnapshot.store(nullptr, std::memory_order_seq_cst);
}

void MixBus::finalizeOutput(AudioBuffer& buffer) noexcept
{
    float** samples = buffer.raw_ptrs();
    if ( samples ) {
        const std::size_t frameCount = buffer.num_frames();
        switch ( get_channel_mode() ) {
        case MixBusChannelMode::MuteLeft:
            if ( buffer.num_channels() > 0U ) {
                std::memset(samples[0], 0, frameCount * sizeof(float));
            }
            break;
        case MixBusChannelMode::MuteRight:
            if ( buffer.num_channels() > 1U ) {
                std::memset(samples[1], 0, frameCount * sizeof(float));
            }
            break;
        case MixBusChannelMode::CopyLeftToRight:
            if ( buffer.num_channels() > 1U ) {
                std::memcpy(samples[1], samples[0], frameCount * sizeof(float));
            }
            break;
        case MixBusChannelMode::CopyRightToLeft:
            if ( buffer.num_channels() > 1U ) {
                std::memcpy(samples[0], samples[1], frameCount * sizeof(float));
            }
            break;
        case MixBusChannelMode::Stereo: break;
        }
    }

    float maxLeft{ 0.0F };
    float maxRight{ 0.0F };
    if ( samples ) {
        if ( buffer.num_channels() > 0U ) {
            for ( std::size_t frame = 0U; frame < buffer.num_frames();
                  ++frame ) {
                maxLeft = std::max(maxLeft, std::abs(samples[0][frame]));
            }
        }
        if ( buffer.num_channels() > 1U ) {
            for ( std::size_t frame = 0U; frame < buffer.num_frames();
                  ++frame ) {
                maxRight = std::max(maxRight, std::abs(samples[1][frame]));
            }
        } else if ( buffer.num_channels() > 0U ) {
            maxRight = maxLeft;
        }
    }

    const float previousLeft  = m_leftLevel.load(std::memory_order_relaxed);
    const float previousRight = m_rightLevel.load(std::memory_order_relaxed);
    m_leftLevel.store(maxLeft > previousLeft ? maxLeft : previousLeft * 0.95F,
                      std::memory_order_relaxed);
    m_rightLevel.store(
        maxRight > previousRight ? maxRight : previousRight * 0.95F,
        std::memory_order_relaxed);
}
}  // namespace ice
