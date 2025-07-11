#ifndef ICE_AUDIOBUFFER_HPP
#define ICE_AUDIOBUFFER_HPP

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

#include "ice/manage/AudioFormat.hpp"

namespace ice {
class AudioBuffer {
   public:
    AudioDataFormat afmt;
    using ChannelData = std::vector<float>;

    // 构造AudioBuffer
    // 指定格式和帧数
    AudioBuffer(const AudioBuffer& cp) = delete;
    AudioBuffer() = default;

    AudioBuffer(const AudioDataFormat& format, size_t num_frames = 0);
    void resize(const AudioDataFormat& format, size_t num_frames);

    // 清理样本为 0
    inline void clear() {
        if (!data.empty()) {
            for (auto& channeldata : data) {
                for (auto& channel_data : data) {
                    // 使用 std::fill 将这个声道的内存清零
                    std::fill(channel_data.begin(), channel_data.end(), 0.0f);
                }
            }
        }
    }

    // 数据访问
    inline auto write_pointer(uint16_t channel_index) {
        return data[channel_index].data();
    }
    inline auto read_pointer(uint16_t channel_index) const {
        return data[channel_index].data();
    }

    inline auto num_frames() const { return data.empty() ? 0 : data[0].size(); }

    inline void operator+=(const AudioBuffer& other) {
        if (afmt != other.afmt || num_frames() != other.num_frames()) {
            throw std::runtime_error("用于混合的AudioBuffer格式不匹配");
        }
        auto frames = num_frames();
        for (uint16_t ch = 0; ch < afmt.channels; ++ch) {
            auto dest = write_pointer(ch);
            auto src = other.read_pointer(ch);
            // CPU紧凑循环。
            for (uint32_t i = 0; i < frames; ++i) {
                dest[i] += src[i];
            }
        }
    }

   protected:
    std::vector<ChannelData> data;
};
}  // namespace ice

#endif  // ICE_AUDIOBUFFER_HPP
