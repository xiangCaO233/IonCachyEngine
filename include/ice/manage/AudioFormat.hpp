#ifndef ICE_AUDIOFORMAT_HPP
#define ICE_AUDIOFORMAT_HPP

#include <cstdint>

namespace ice {
struct AudioFormat {
    uint16_t channels{2};
    uint32_t samplerate{48000};
    bool operator==(const AudioFormat& other) const = default;
};
}  // namespace ice

#endif  // ICE_AUDIOFORMAT_HPP
