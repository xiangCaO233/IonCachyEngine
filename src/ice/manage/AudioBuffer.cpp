#include <ice/manage/AudioBuffer.hpp>
#include <vector>

namespace ice
{
#ifndef __linux__
/// @brief 在控制阶段为每声道建立独立存储。
/// @warning resize 会分配，不能从音频回调首次创建该缓冲。
AudioBuffer::AudioBuffer(const AudioDataFormat& format, size_t num_frames)
    : afmt(format)
{
    // 与默认构造不同，此入口立即建立指定格式和逻辑帧数。
    resize(format, num_frames);
}
/// @brief 接管非 Linux 声道数组及其地址表，不复制 PCM。
/// 已移动的 vector 保留原存储地址，因此指针表可一起转移。
AudioBuffer::AudioBuffer(AudioBuffer&& other) noexcept
    : afmt(other.afmt)
    , _data(std::move(other._data))
    , channel_pointers_(std::move(other.channel_pointers_))
    , m_activeFrames(other.m_activeFrames)
    , m_frameCapacity(other.m_frameCapacity)
{
    // 源对象不再描述可消费音频，容量与有效长度也同步清空。
    other.afmt.channels   = 0;
    other.m_activeFrames  = 0U;
    other.m_frameCapacity = 0U;
}
/// @brief 替换目标存储，并使源对象变为逻辑空缓冲。
/// @warning 赋值可释放目标旧存储，应放在控制路径。
AudioBuffer& AudioBuffer::operator=(AudioBuffer&& other) noexcept
{
    // 自移动不能先转移存储再清零自身元信息。
    if ( this != &other ) {
        // 声道数组与地址表成对转移，不能单独保留目标原地址表。
        afmt                = other.afmt;
        _data               = std::move(other._data);
        channel_pointers_   = std::move(other.channel_pointers_);
        other.afmt.channels = 0;
        // 容量与活动长度分别保留，移动后仍可在原准备上限内切换块长。
        m_activeFrames        = other.m_activeFrames;
        m_frameCapacity       = other.m_frameCapacity;
        other.m_activeFrames  = 0U;
        other.m_frameCapacity = 0U;
    }
    return *this;
}
/// @brief 按格式重设各声道长度，并刷新借用地址表。
/// @warning 容量可能增长；实时路径应使用 set_active_frames。
void AudioBuffer::resize(const AudioDataFormat& format, size_t num_frames)
{
    afmt = format;
    // 为每个声道调整大小，这会导致多次独立的内存分配
    // 声道数减小时销毁多余声道，增大时建立新的独立数组。
    _data.resize(afmt.channels);
    for ( auto& channel_data : _data ) {
        // 每声道的已有前缀由 vector 保留，新增长度按其值初始化语义处理。
        channel_data.resize(num_frames);
    }
    // 这里重设准备上限；逻辑短块应单独用 set_active_frames 表达。
    m_activeFrames  = num_frames;
    m_frameCapacity = num_frames;
    // 任一声道 resize 都可能搬迁存储，必须在全部调整后重建指针。
    // 更新指针数组
    sync_pointers();
}
/// @brief 将各声道 vector 的地址同步到平面 PCM 指针表。
/// @warning 指针表本身可扩容，只在存储准备或改变后执行。
void AudioBuffer::sync_pointers()
{
    // 表中指针均不拥有样本，失效规则由对应声道 vector 决定。
    channel_pointers_.resize(_data.size());
    for ( size_t i = 0; i < _data.size(); ++i ) {
        // 此处仅建立借用映射，PCM 的所有权始终保存在 _data 中。
        channel_pointers_[i] = _data[i].data();
    }
}
// 与上方非 Linux 实现保持相同的逻辑帧数与所有权语义。
#else
/// @brief 在 Linux 上建立按 SIMD 对齐跨度排列的连续声道存储。
/// @warning 控制路径分配；对齐容量可能大于调用方请求的逻辑帧数。
/// @param format 后续解释采样时使用的声道数及采样率。
/// @param num_frames 每声道请求帧数，不是全部声道的采样总数。
AudioBuffer::AudioBuffer(const AudioDataFormat& format, size_t num_frames)
    : afmt(format)
{
    // 对齐分配与声道跨度计算由头文件中的 Linux resize 统一完成。
    resize(format, num_frames);
}
/// @brief 转移对齐分配的连续块及声道地址表，不重新排列 PCM。
/// 逻辑帧数、对齐帧数、固定存储跨度必须一起转移。
AudioBuffer::AudioBuffer(AudioBuffer&& other) noexcept
    : afmt(other.afmt)
    , _contiguous_buffer(std::move(other._contiguous_buffer))
    , channel_pointers_(std::move(other.channel_pointers_))
    , _original_num_frames(other._original_num_frames)
    , _aligned_num_frames(other._aligned_num_frames)
    , _storage_aligned_num_frames(other._storage_aligned_num_frames)
    , _frame_capacity(other._frame_capacity)
{
    // vector 已转移所有权；这里清空的是源对象对外暴露的帧数和格式。
    // 不能仅清逻辑长度而留下可被误用的旧声道跨度。
    // 这里只取消源的逻辑音频描述，不对已经转移的样本再执行清零。
    other._original_num_frames        = 0;
    other._aligned_num_frames         = 0;
    other._storage_aligned_num_frames = 0;
    other._frame_capacity             = 0;
    other.afmt.channels               = 0;
}
/// @brief 接管另一连续缓冲，同时替换所有用于定位声道的元信息。
/// @warning 旧目标内存可能在赋值时释放，不能当成实时无分配操作。
AudioBuffer& AudioBuffer::operator=(AudioBuffer&& other) noexcept
{
    if ( this != &other ) {
        // 目标原内存由移动赋值释放，之后借用地址表指向接管后的内存。
        afmt               = other.afmt;
        _contiguous_buffer = std::move(other._contiguous_buffer);
        channel_pointers_  = std::move(other.channel_pointers_);
        // 有效长度控制消费范围，对齐长度控制 SIMD 处理尾部，两者不可混用。
        _original_num_frames = other._original_num_frames;
        _aligned_num_frames  = other._aligned_num_frames;
        // 存储跨度可能大于当前块长，不能根据有效帧数重新推导声道地址。
        _storage_aligned_num_frames = other._storage_aligned_num_frames;
        // 准备上限不随最后一次短块而缩小，移动后继续保留该上限。
        _frame_capacity = other._frame_capacity;

        // 源对象继续可析构，但不再提供有效的 PCM 帧或声道。
        other._original_num_frames        = 0;
        other._aligned_num_frames         = 0;
        other._storage_aligned_num_frames = 0;
        other._frame_capacity             = 0;
        other.afmt.channels               = 0;
    }
    return *this;
}
#endif  // __linux__ 平台存储分支
}  // namespace ice
