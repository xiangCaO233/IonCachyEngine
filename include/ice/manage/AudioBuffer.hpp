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
        for (auto& channel_data : _data) {
            std::fill(channel_data.begin(), channel_data.end(), 0.0f);
        }
    }

    // 数据访问
    inline auto raw_ptrs() {
        return _data.empty() ? nullptr : channel_pointers_.data();
    }
    // 常量重载
    inline auto raw_ptrs() const {
        return reinterpret_cast<const float* const*>(
            _data.empty() ? nullptr : channel_pointers_.data());
    }

    inline auto num_frames() const {
        return _data.empty() ? 0 : _data[0].size();
    }

    inline void operator+=(const AudioBuffer& other) {
        if (afmt != other.afmt || num_frames() != other.num_frames()) {
            throw std::runtime_error("用于混合的AudioBuffer格式不匹配");
        }
        auto frames = num_frames();
        for (uint16_t ch = 0; ch < afmt.channels; ++ch) {
            // 获取第 ch 声道的 float* 起始指针
            float* dest_channel = this->raw_ptrs()[ch];
            // 另一个buffer只读
            const float* src_channel = other.raw_ptrs()[ch];

            // 在这个声道上进行紧凑循环
            for (size_t i = 0; i < frames; ++i) {
                dest_channel[i] += src_channel[i];
            }
        }
    }

   protected:
    // 缓冲区本体
    std::vector<ChannelData> _data;
    // 指针管理
    std::vector<float*> channel_pointers_;

    // 同步指针管理
    void sync_pointers();
};
}  // namespace ice

#endif  // ICE_AUDIOBUFFER_HPP
