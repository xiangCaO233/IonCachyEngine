#include <ice/manage/AudioBuffer.hpp>

#include <algorithm>
#include <cstddef>
#include <utility>
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
/// @param format 新格式，零声道建立无活动样本的空状态。
/// @param num_frames 每声道存储长度，不包含其他声道的样本数。
/// @return 尺寸超界时返回 false，成功时返回 true；分配仍沿用异常失败通道。
/// @details
/// 扩容成功前保留原格式、样本及指针表，控制侧可在失败后继续持有旧状态。
/// @warning 容量可能增长；实时路径应使用 set_active_frames。
bool AudioBuffer::resize(const AudioDataFormat& format, size_t num_frames)
{
    // 零声道与 Linux 分支保持同一空状态，不能激活不存在的样本平面。
    if ( format.channels == 0 ) num_frames = 0;
    // 在改变旧格式与平面前拒绝不可表达的总元素数，避免触发长度异常。
    if ( format.channels != 0 &&
         num_frames > ChannelData{}.max_size() / format.channels )
        return false;
    // 已有声道的容量都足够时，调整 float 数组不会分配，可直接复用旧地址。
    // 缩减声道只释放多余存储；指针表容量已覆盖原声道数，也无需扩容。
    bool canReuse = format.channels <= _data.size();
    for ( size_t channel = 0; canReuse && channel < format.channels; ++channel )
        canReuse = _data[channel].capacity() >= num_frames;
    if ( canReuse ) {
        _data.resize(format.channels);
        for ( auto& channelData : _data ) channelData.resize(num_frames);
        // 活动帧数由下方统一提交，表先刷新到调整后的实际存储。
        sync_pointers();
    } else {
        // 必须分配时准备完整替代状态，任何一步失败都不触及原样本与借用表。
        // 峰值内存包含新旧两组声道，只用于控制阶段真实扩容，不用于逻辑短块。
        std::vector<ChannelData> preparedData(format.channels);
        std::vector<float*>      preparedPointers(format.channels);
        for ( size_t channel = 0; channel < format.channels; ++channel ) {
            auto& data = preparedData[channel];
            data.resize(num_frames);
            // 与原逐声道 resize 一致，保留有效存储前缀，新增样本由 vector
            // 清零。 新增声道没有旧前缀；缩短时不访问被裁掉的尾部。
            if ( channel < _data.size() )
                std::copy_n(_data[channel].begin(),
                            std::min(num_frames, _data[channel].size()),
                            data.begin());
            preparedPointers[channel] = data.data();
        }
        // 标准分配器的容器交换不申请存储，两个拥有关系成对提交。
        _data.swap(preparedData);
        channel_pointers_.swap(preparedPointers);
        // 临时容器在此释放原存储，失败路径从未交换，因此旧借用始终有效。
    }
    // 从此处起只有标量赋值，不会在格式已更新后再发生分配失败。
    afmt            = format;
    m_activeFrames  = num_frames;
    m_frameCapacity = num_frames;
    return true;
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
    , m_pointerCapacity(std::exchange(other.m_pointerCapacity, 0))
    , _original_num_frames(other._original_num_frames)
    , _aligned_num_frames(other._aligned_num_frames)
    , _storage_aligned_num_frames(other._storage_aligned_num_frames)
    , _frame_capacity(other._frame_capacity)
{
    // 存储已转移所有权；这里清空的是源对象对外暴露的帧数和格式。
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
        // 表容量必须跟随所有权转移，源复用时不能误判为已有存储。
        m_pointerCapacity = std::exchange(other.m_pointerCapacity, 0);
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
