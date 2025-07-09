#include <ice/manage/AudioBuffer.hpp>
#include <stdexcept>
#include <vector>

namespace ice {
// 构造AudioBuffer
// 指定声道数和帧数
AudioBuffer::AudioBuffer(size_t num_channels, size_t num_frames) {
    data = std::vector<ChannelData>(num_channels, ChannelData(num_frames));
}

void AudioBuffer::resize(size_t num_channels, size_t num_frames) {
    // 调整外层 vector 的大小（即声道数）
    data.resize(num_channels);

    // 调整每个内层 vector 的大小（即每个声道的帧数）
    for (auto& channel_data : data) {
        channel_data.resize(num_frames);
    }
}

// 清理样本为 0
void AudioBuffer::clear() {
    if (!data.empty()) {
        for (auto& channeldata : data) {
            for (auto& channel_data : data) {
                // 使用 std::fill 将这个声道的内存清零
                std::fill(channel_data.begin(), channel_data.end(), 0.0f);
            }
        }
    }
}

void AudioBuffer::operator+=(const AudioBuffer& other) {
    // 你的尺寸检查非常完美，保留它。
    if (get_num_channels() != other.get_num_channels() ||
        get_num_frames() != other.get_num_frames()) {
        throw std::runtime_error("用于混合的AudioBuffer格式不匹配。");
    }

    const auto num_channels = get_num_channels();
    const auto num_frames = get_num_frames();

    for (uint16_t ch = 0; ch < num_channels; ++ch) {
        float* dest = get_write_pointer(ch);
        const float* src = other.get_read_pointer(ch);

        // 这是CPU将花费大部分时间的紧凑循环。
        for (uint32_t i = 0; i < num_frames; ++i) {
            dest[i] += src[i];
        }
    }
}

}  // namespace ice
