#ifndef ICE_AUDIOBUFFER_HPP
#define ICE_AUDIOBUFFER_HPP

#include <cstddef>
#include <cstdint>
#include <vector>

namespace ice {
class AudioBuffer {
   public:
    using ChannelData = std::vector<float>;

    // 构造AudioBuffer
    // 指定声道数和帧数
    AudioBuffer(size_t num_channels = 0, size_t num_frames = 0);

    void resize(size_t num_channels, size_t num_frames);
    // 清理样本为 0
    void clear();

    // 数据访问
    inline float* get_write_pointer(uint16_t channel_index) {
        return data[channel_index].data();
    }
    inline const float* get_read_pointer(uint16_t channel_index) const {
        return data[channel_index].data();
    }

    inline auto get_num_channels() const { return data.size(); }
    inline auto get_num_frames() const {
        return data.empty() ? 0 : data[0].size();
    }

    void operator+=(const AudioBuffer& other);

   protected:
    std::vector<ChannelData> data;
};
}  // namespace ice

#endif  // ICE_AUDIOBUFFER_HPP
