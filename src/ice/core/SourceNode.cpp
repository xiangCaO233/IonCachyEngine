#include "ice/core/SourceNode.hpp"

#include "ice/config/config.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

namespace ice
{
struct SourceNode::ReferenceProviderState {
    /// @brief 不拥有的轻量 provider 上下文。
    const void* context{ nullptr };

    /// @brief 轻量 provider 读取函数。
    ReferencePositionReader reader{ nullptr };

    /// @brief 兼容旧接口的不可变 std::function。
    std::function<std::size_t()> compatibilityProvider;

    /// @brief 查询状态是否包含有效 provider。
    /// @return 任一 provider 有效时返回 true。
    bool valid() const noexcept
    {
        return reader != nullptr || static_cast<bool>(compatibilityProvider);
    }

    /// @brief 读取当前参考帧。
    /// @return provider 未设置时返回零。
    /// @warning 音频回调热路径；兼容 provider 由调用方保证无异常和无分配。
    std::size_t read() const
    {
        if ( reader ) return reader(context);
        if ( compatibilityProvider ) return compatibilityProvider();
        return 0U;
    }
};

SourceNode::SourceNode(std::shared_ptr<AudioTrack> track)
    : m_track(std::move(track))
{
    if ( m_track ) {
        m_totalFrames = m_track->num_frames();
    }

    auto initialProvider = std::make_unique<ReferenceProviderState>();
    m_activeProvider.store(initialProvider.get(), std::memory_order_seq_cst);
    m_activeProviderOwner = std::move(initialProvider);
}

SourceNode::~SourceNode() = default;

void SourceNode::process(AudioBuffer& buffer)
{
    buffer.clear();
    if ( !m_track || !m_isPlaying.load(std::memory_order_acquire) ) return;

    if ( buffer.afmt != ICEConfig::internal_format ) {
        m_rejectedProcessCount.fetch_add(1U, std::memory_order_relaxed);
        updateLevels(buffer, false);
        return;
    }

    const std::size_t requestedFrames = buffer.num_frames();
    if ( requestedFrames == 0U ) {
        updateLevels(buffer, false);
        return;
    }

    std::size_t gainedThisBlock{ 0U };
    std::size_t silenceFrames{ 0U };
    bool        startedInsideBlock{ false };

    const std::size_t relativeDelay =
        m_scheduledStartDelayFrames.load(std::memory_order_relaxed);
    if ( relativeDelay > 0U ) {
        if ( relativeDelay >= requestedFrames ) {
            m_scheduledStartDelayFrames.store(relativeDelay - requestedFrames,
                                              std::memory_order_relaxed);
            updateLevels(buffer, false);
            return;
        }
        silenceFrames = relativeDelay;
        m_scheduledStartDelayFrames.store(0U, std::memory_order_relaxed);
        startedInsideBlock = true;
    } else if ( const std::size_t scheduledStart =
                    m_scheduledStartFrame.load(std::memory_order_relaxed);
                scheduledStart > 0U ) {
        const ReferenceProviderState* provider = acquireReferenceProvider();
        const bool        providerValid        = provider && provider->valid();
        const std::size_t currentReference =
            providerValid ? provider->read() : 0U;
        releaseReferenceProvider();

        if ( !providerValid ) {
            updateLevels(buffer, false);
            return;
        }

        if ( currentReference < scheduledStart ) {
            const std::size_t framesToWait = scheduledStart - currentReference;
            if ( framesToWait >= requestedFrames ) {
                updateLevels(buffer, false);
                return;
            }

            silenceFrames = framesToWait;
            m_scheduledStartFrame.store(0U, std::memory_order_relaxed);
            startedInsideBlock = true;
        } else {
            m_scheduledStartFrame.store(0U, std::memory_order_relaxed);
        }
    }

    if ( startedInsideBlock ) {
        const std::size_t framesToRead = requestedFrames - silenceFrames;
        const std::size_t playbackPosition =
            m_playbackPosition.load(std::memory_order_relaxed);
        gainedThisBlock = m_track->read(buffer, playbackPosition, framesToRead);
        if ( gainedThisBlock > framesToRead ) {
            gainedThisBlock = framesToRead;
        }
        shiftDecodedFrames(buffer, silenceFrames, gainedThisBlock);
        m_playbackPosition.store(playbackPosition + gainedThisBlock,
                                 std::memory_order_relaxed);
    } else if ( m_scheduledStartFrame.load(std::memory_order_relaxed) == 0U ) {
        const std::size_t playbackPosition =
            m_playbackPosition.load(std::memory_order_relaxed);
        gainedThisBlock =
            m_track->read(buffer, playbackPosition, requestedFrames);
        if ( gainedThisBlock > requestedFrames ) {
            gainedThisBlock = requestedFrames;
        }
        if ( gainedThisBlock < requestedFrames ) {
            buffer.clear_from(gainedThisBlock);
        }
        m_playbackPosition.store(playbackPosition + gainedThisBlock,
                                 std::memory_order_relaxed);
    }

    const std::size_t playbackPosition =
        m_playbackPosition.load(std::memory_order_relaxed);
    const bool reachedEnd = playbackPosition >= m_totalFrames;
    if ( reachedEnd ) {
        const bool looping = m_isLooping.load(std::memory_order_relaxed);
        if ( looping ) {
            m_playbackPosition.store(0U, std::memory_order_relaxed);
        } else {
            m_playbackPosition.store(m_totalFrames, std::memory_order_relaxed);
            pause();
        }

        for ( const auto& callback : m_callbacks ) {
            callback->play_done(looping);
        }

        if ( !looping ) {
            notifyFinalInput();
        }

        if ( gainedThisBlock == 0U ) {
            updateLevels(buffer, false);
            return;
        }
    }

    const std::size_t publishedPosition =
        m_playbackPosition.load(std::memory_order_relaxed);
    for ( const auto& callback : m_callbacks ) {
        callback->frameplaypos_updated(publishedPosition);
        using DoubleSeconds = std::chrono::duration<double>;
        callback->timeplaypos_updated(
            std::chrono::duration_cast<std::chrono::nanoseconds>(DoubleSeconds(
                static_cast<double>(publishedPosition) /
                static_cast<double>(ICEConfig::internal_format.samplerate))));
    }

    const float gain = m_volume.load(std::memory_order_relaxed);
    if ( std::abs(gain - 1.0F) > std::numeric_limits<float>::epsilon() ) {
        applyVolume(buffer, gain);
    }
    updateLevels(buffer, gainedThisBlock > 0U);
}

void SourceNode::set_reference_pos_provider(
    std::function<std::size_t()> provider)
{
    auto state                   = std::make_unique<ReferenceProviderState>();
    state->compatibilityProvider = std::move(provider);
    publishReferenceProvider(std::move(state));
}

void SourceNode::set_reference_pos_provider(const void*             context,
                                            ReferencePositionReader reader)
{
    auto state     = std::make_unique<ReferenceProviderState>();
    state->context = reader ? context : nullptr;
    state->reader  = reader;
    publishReferenceProvider(std::move(state));
}

void SourceNode::clear_reference_pos_provider()
{
    publishReferenceProvider(std::make_unique<ReferenceProviderState>());
}

void SourceNode::reclaimRetiredReferenceProviders()
{
    std::lock_guard<std::mutex> lock(m_providerControlMutex);
    reclaimRetiredReferenceProvidersLocked();
}

std::size_t SourceNode::retiredReferenceProviderCount() const
{
    std::lock_guard<std::mutex> lock(m_providerControlMutex);
    return m_retiredProviders.size();
}

void SourceNode::publishReferenceProvider(
    std::unique_ptr<ReferenceProviderState> state)
{
    if ( !state ) {
        state = std::make_unique<ReferenceProviderState>();
    }

    std::lock_guard<std::mutex>   lock(m_providerControlMutex);
    const ReferenceProviderState* nextAddress = state.get();
    if ( m_activeProviderOwner ) {
        m_retiredProviders.push_back(std::move(m_activeProviderOwner));
    }
    m_activeProviderOwner = std::move(state);
    m_activeProvider.store(nextAddress, std::memory_order_seq_cst);
    reclaimRetiredReferenceProvidersLocked();
}

void SourceNode::reclaimRetiredReferenceProvidersLocked()
{
    const ReferenceProviderState* protectedProvider =
        m_providerHazard.load(std::memory_order_seq_cst);
    std::erase_if(m_retiredProviders,
                  [protectedProvider](
                      const std::unique_ptr<ReferenceProviderState>& provider) {
                      return provider.get() != protectedProvider;
                  });
}

const SourceNode::ReferenceProviderState*
SourceNode::acquireReferenceProvider() noexcept
{
    const ReferenceProviderState* provider{ nullptr };
    do {
        provider = m_activeProvider.load(std::memory_order_seq_cst);
        m_providerHazard.store(provider, std::memory_order_seq_cst);
    } while ( provider != m_activeProvider.load(std::memory_order_seq_cst) );
    return provider;
}

void SourceNode::releaseReferenceProvider() noexcept
{
    m_providerHazard.store(nullptr, std::memory_order_seq_cst);
}

void SourceNode::notifyFinalInput() noexcept
{
    if ( m_finalInputNotified.exchange(true, std::memory_order_acq_rel) ) {
        return;
    }
    if ( m_finalInputListener ) {
        m_finalInputListener(m_finalInputListenerContext);
    }
}

void SourceNode::shiftDecodedFrames(AudioBuffer& buffer,
                                    std::size_t  silenceFrames,
                                    std::size_t  decodedFrames) noexcept
{
    float** samples = buffer.raw_ptrs();
    if ( !samples || silenceFrames >= buffer.num_frames() ) return;

    const std::size_t safeDecodedFrames =
        std::min(decodedFrames, buffer.num_frames() - silenceFrames);
    for ( std::uint16_t channel = 0U; channel < buffer.num_channels();
          ++channel ) {
        if ( safeDecodedFrames > 0U ) {
            std::memmove(samples[channel] + silenceFrames,
                         samples[channel],
                         safeDecodedFrames * sizeof(float));
        }
        std::memset(samples[channel], 0, silenceFrames * sizeof(float));
    }
}

void SourceNode::applyVolume(AudioBuffer& buffer, float gain) noexcept
{
    float** samples = buffer.raw_ptrs();
    if ( !samples ) return;
    for ( std::uint16_t channel = 0U; channel < buffer.num_channels();
          ++channel ) {
        for ( std::size_t frame = 0U; frame < buffer.num_frames(); ++frame ) {
            samples[channel][frame] *= gain;
        }
    }
}

void SourceNode::updateLevels(const AudioBuffer& buffer, bool audible) noexcept
{
    float               maxLeft{ 0.0F };
    float               maxRight{ 0.0F };
    const float* const* samples = buffer.raw_ptrs();
    if ( audible && samples ) {
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
