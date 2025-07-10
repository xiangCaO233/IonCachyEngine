#include <ice/manage/AudioBuffer.hpp>
#include <vector>

namespace ice {
// 构造AudioBuffer
// 指定声道数和帧数
AudioBuffer::AudioBuffer(const AudioFormat &format, size_t num_frames)
    : afmt(format) {
    data = std::vector<ChannelData>(afmt.channels, ChannelData(num_frames));
}

void AudioBuffer::resize(const AudioFormat &format, size_t num_frames) {
    afmt = format;
    // 调整外层 vector 的大小（即声道数）
    data.resize(afmt.channels);

    // 调整每个内层 vector 的大小（即每个声道的帧数）
    for (auto &channel_data : data) {
        channel_data.resize(num_frames);
    }
}

}  // namespace ice
