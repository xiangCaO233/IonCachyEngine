#include <ice/manage/AudioBuffer.hpp>
#include <vector>

namespace ice
{
#ifndef __linux__
// 构造函数
AudioBuffer::AudioBuffer(const AudioDataFormat& format, size_t num_frames)
    : afmt(format)
{
    resize(format, num_frames);
}
// 移动构造函数
AudioBuffer::AudioBuffer(AudioBuffer&& other) noexcept
    : afmt(other.afmt)
    , _data(std::move(other._data))
    , channel_pointers_(std::move(other.channel_pointers_))
{
    other.afmt.channels = 0;
}
// 移动赋值运算符
AudioBuffer& AudioBuffer::operator=(AudioBuffer&& other) noexcept
{
    if ( this != &other )
        {
            afmt                = other.afmt;
            _data               = std::move(other._data);
            channel_pointers_   = std::move(other.channel_pointers_);
            other.afmt.channels = 0;
        }
    return *this;
}
// resize 实现
void AudioBuffer::resize(const AudioDataFormat& format, size_t num_frames)
{
    afmt = format;
    // 为每个声道调整大小，这会导致多次独立的内存分配
    _data.resize(afmt.channels);
    for ( auto& channel_data : _data )
        {
            channel_data.resize(num_frames);
        }
    // 更新指针数组
    sync_pointers();
}
// sync_pointers 实现
void AudioBuffer::sync_pointers()
{
    channel_pointers_.resize(_data.size());
    for ( size_t i = 0; i < _data.size(); ++i )
        {
            channel_pointers_[i] = _data[i].data();
        }
}
#else
// 构造AudioBuffer
// 指定声道数和帧数
AudioBuffer::AudioBuffer(const AudioDataFormat& format, size_t num_frames)
    : afmt(format)
{
    resize(format, num_frames);
}
// 高效的移动构造函数
AudioBuffer::AudioBuffer(AudioBuffer&& other) noexcept
    : afmt(other.afmt)
    , _contiguous_buffer(std::move(other._contiguous_buffer))
    , channel_pointers_(std::move(other.channel_pointers_))
    , _original_num_frames(other._original_num_frames)
    , _aligned_num_frames(other._aligned_num_frames)
{
    // 清空源对象，防止双重释放
    other._original_num_frames = 0;
    other._aligned_num_frames  = 0;
    other.afmt.channels        = 0;
}
// 高效的移动赋值运算符
AudioBuffer& AudioBuffer::operator=(AudioBuffer&& other) noexcept
{
    if ( this != &other )
        {
            afmt                 = other.afmt;
            _contiguous_buffer   = std::move(other._contiguous_buffer);
            channel_pointers_    = std::move(other.channel_pointers_);
            _original_num_frames = other._original_num_frames;
            _aligned_num_frames  = other._aligned_num_frames;

            other._original_num_frames = 0;
            other._aligned_num_frames  = 0;
            other.afmt.channels        = 0;
        }
    return *this;
}
#endif  //__APPLE__
}  // namespace ice
