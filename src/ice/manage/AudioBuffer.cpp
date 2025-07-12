#include <ice/manage/AudioBuffer.hpp>
#include <vector>

namespace ice {
// 构造AudioBuffer
// 指定声道数和帧数
AudioBuffer::AudioBuffer(const AudioDataFormat& format, size_t num_frames)
    : afmt(format) {
    resize(format, num_frames);
}
// 高效的移动构造函数
AudioBuffer::AudioBuffer(AudioBuffer&& other) noexcept
    : afmt(other.afmt),
      _contiguous_buffer(std::move(other._contiguous_buffer)),
      channel_pointers_(std::move(other.channel_pointers_)),
      _original_num_frames(other._original_num_frames),
      _aligned_num_frames(other._aligned_num_frames) {
    // 清空源对象，防止双重释放
    other._original_num_frames = 0;
    other._aligned_num_frames = 0;
    other.afmt.channels = 0;
}
// 高效的移动赋值运算符
AudioBuffer& AudioBuffer::operator=(AudioBuffer&& other) noexcept {
    if (this != &other) {
        afmt = other.afmt;
        _contiguous_buffer = std::move(other._contiguous_buffer);
        channel_pointers_ = std::move(other.channel_pointers_);
        _original_num_frames = other._original_num_frames;
        _aligned_num_frames = other._aligned_num_frames;

        other._original_num_frames = 0;
        other._aligned_num_frames = 0;
        other.afmt.channels = 0;
    }
    return *this;
}
}  // namespace ice
