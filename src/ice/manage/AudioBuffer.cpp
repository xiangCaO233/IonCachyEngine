#include <ice/manage/AudioBuffer.hpp>
#include <vector>

namespace ice {
// 构造AudioBuffer
// 指定声道数和帧数
AudioBuffer::AudioBuffer(const AudioDataFormat &format, size_t num_frames)
    : afmt(format) {
    resize(format, num_frames);
}

void AudioBuffer::resize(const AudioDataFormat &format, size_t num_frames) {
    afmt = format;
    _data.resize(afmt.channels);

    for (auto &channel_data : _data) {
        channel_data.resize(num_frames);
    }

    sync_pointers();
}
void AudioBuffer::sync_pointers() {
    channel_pointers_.resize(_data.size());
    for (size_t i = 0; i < _data.size(); ++i) {
        channel_pointers_[i] = _data[i].data();
    }
}

}  // namespace ice
