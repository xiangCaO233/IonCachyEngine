#ifndef ICE_AUDIOBUFFER_HPP
#define ICE_AUDIOBUFFER_HPP

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <ice/execptions/buffer_error.hpp>
#include <ice/manage/AudioFormat.hpp>
#include <vector>

#ifdef __clang__
#    include <algorithm>
#endif  //__clang__
namespace ice
{
#ifndef __linux__
/// @brief 非 Linux 的平面浮点 PCM 缓冲，每声道独立持有连续数组。
/// 格式和容量只允许在控制阶段改变，消费方借用指针不拥有样本。
class AudioBuffer
{
public:
    /// @brief 当前样本解释格式，不能绕过 resize 单独改变声道数。
    AudioDataFormat afmt;
    /// @brief 默认对象不准备音频存储。
    AudioBuffer() = default;
    AudioBuffer(const AudioDataFormat& format, size_t num_frames = 0);

    // 删除拷贝，实现移动
    AudioBuffer(const AudioBuffer&)            = delete;
    AudioBuffer& operator=(const AudioBuffer&) = delete;
    AudioBuffer(AudioBuffer&& other) noexcept;
    AudioBuffer& operator=(AudioBuffer&& other) noexcept;

    /// @brief 控制阶段调整存储大小，可能使所有旧借用地址失效。
    void resize(const AudioDataFormat& format, size_t num_frames);

    /// @brief 在不改变已分配存储的前提下设置逻辑帧数。
    /// @param numFrames 新逻辑帧数。
    /// @return 未超过预分配容量时返回 true。
    /// @warning 音频回调热路径：本函数不分配内存。
    bool set_active_frames(size_t numFrames)
    {
        // 只改变对外可见长度，不能把较短块误当成需要重新分配的缓冲。
        if ( numFrames > m_frameCapacity ) return false;
        m_activeFrames = numFrames;
        return true;
    }

    /// @brief 获取当前存储能够容纳的最大帧数。
    /// @return 每声道最大帧数。
    [[nodiscard]] size_t frame_capacity() const { return m_frameCapacity; }

    /// @brief 清零全部已准备存储，不只清理当前有效块。
    /// @warning 音频块操作，只遍历既有容量，不允许在此扩容。
    void clear()
    {
        for ( auto& channel_data : _data ) {
            std::fill(channel_data.begin(), channel_data.end(), 0.0f);
        }
    }
    /// @brief 从指定帧清零到有效块末尾，保留已有有效前缀。
    /// @warning 音频热路径，只访问已准备存储。
    void clear_from(size_t start_frame)
    {
        if ( start_frame >= m_activeFrames ) return;
        for ( auto& channel_data : _data ) {
            std::fill(channel_data.begin() + start_frame,
                      channel_data.begin() + m_activeFrames,
                      0.0f);
        }
    }
    /// @brief 借用声道指针表；resize 或缓冲销毁后不得继续使用旧地址。
    float** raw_ptrs()
    {
        return channel_pointers_.empty() ? nullptr : channel_pointers_.data();
    }
    /// @brief 借用只读声道地址，不能通过该视图改变样本或指针表。
    const float* const* raw_ptrs() const
    {
        return channel_pointers_.empty()
                   ? nullptr
                   : reinterpret_cast<const float* const*>(
                         channel_pointers_.data());
    }
    // 信息查询
    /// @brief 返回当前有效长度，不能用它推断声道分配地址之间的间隔。
    size_t num_frames() const { return m_activeFrames; }
    /// @brief 返回当前格式中的声道数，声道索引须小于此值。
    size_t num_channels() const { return afmt.channels; }
    /// @brief 累加等格式等长度的缓冲，输出不做限幅。
    /// @warning 音频路径调用前必须验证格式，历史错误分支仍抛出异常。
    void operator+=(const AudioBuffer& other)
    {
        if ( afmt != other.afmt || num_frames() != other.num_frames() ) {
            throw ice::buffer_error(
                "AudioBuffer_Baseline format mismatch for mixing.");
        }
        const size_t   frames   = num_frames();
        const uint16_t channels = num_channels();
        for ( uint16_t ch = 0; ch < channels; ++ch ) {
            float*       dest = this->raw_ptrs()[ch];
            const float* src  = other.raw_ptrs()[ch];
            // 只遍历逻辑长度，尾部预留容量不应贡献到本次混音。
            for ( size_t i = 0; i < frames; ++i ) {
                dest[i] += src[i];
            }
        }
    }

private:
    /// @brief 单声道存储，不承诺 SIMD 所需的额外地址对齐。
    using ChannelData = std::vector<float>;
    /// @brief 声道之间不保证相邻，每个声道内部连续。
    std::vector<ChannelData> _data;
    // 2. 依然需要指针数组以匹配接口
    std::vector<float*> channel_pointers_;

    /// @brief 当前对外可见的逻辑帧数。
    size_t m_activeFrames{ 0U };

    /// @brief 每声道已经分配的最大帧数。
    size_t m_frameCapacity{ 0U };

    /// @brief 存储变化后刷新各声道借用地址，调用可能使地址表扩容。
    void sync_pointers();
};
#else
// Linux 混音路径使用受限指针承诺与显式对齐分配。
#    define ICE_RESTRICT __restrict__
#    include <mm_malloc.h>
/// @brief 为连续 PCM 提供固定字节对齐的无状态分配器。
/// @warning 分配和释放只允许发生在控制路径，失败沿用历史异常语义。
template<typename T, size_t Alignment> class AlignedAllocator
{
public:
    using value_type      = T;
    using pointer         = T*;
    using const_pointer   = const T*;
    using reference       = T&;
    using const_reference = const T&;
    using size_type       = std::size_t;
    using difference_type = std::ptrdiff_t;

    /// @brief 容器重绑定元素类型时保持相同的字节对齐约束。
    template<typename U> struct rebind {
        using other = AlignedAllocator<U, Alignment>;
    };

    /// @brief 无状态构造，不预先申请任何样本存储。
    AlignedAllocator() noexcept {}
    /// @brief 元素类型转换不改变分配器对齐策略或引入实例状态。
    template<typename U>
    AlignedAllocator(const AlignedAllocator<U, Alignment>&) noexcept
    {
    }

    /// @brief 按元素数申请对齐存储，不初始化元素内容。
    pointer allocate(size_type n)
    {
        // 先约束元素数乘以字节宽度，防止整数溢出导致申请容量过小。
        if ( n > std::size_t(-1) / sizeof(T) ) {
            throw std::bad_alloc();
        }
        if ( auto p =
                 static_cast<pointer>(_mm_malloc(n * sizeof(T), Alignment)) ) {
            return p;
        }
        throw std::bad_alloc();
    }

    /// @brief 使用与对齐分配匹配的释放入口，大小参数不参与回收。
    void deallocate(pointer p, size_type) noexcept { _mm_free(p); }

    // 分配器不保存实例状态，同类型实例可以相互释放所分配的存储。
    friend bool operator==(const AlignedAllocator&,
                           const AlignedAllocator&) noexcept
    {
        return true;
    }
    friend bool operator!=(const AlignedAllocator&,
                           const AlignedAllocator&) noexcept
    {
        return false;
    }
};

#    include "ice/manage/AudioFormat.hpp"

// SIMD 实现依赖编译目标的 AVX 能力，不在本头做运行时 CPU 派发。
#    include <immintrin.h>

/// @brief Linux 连续平面 PCM 缓冲，声道起点按固定 SIMD 跨度排列。
/// 有效长度可逐块改变，但已有存储的声道间隔只在 resize 时更新。
class AudioBuffer
{
public:
    /// @brief 连续存储的字节对齐要求。
    static constexpr size_t SIMD_ALIGNMENT = 32;
    /// @brief 每个向量容纳的浮点采样数，用于向上取整声道跨度。
    static constexpr size_t SIMD_VECTOR_SIZE = 8;

    /// @brief 样本格式快照，声道布局变更必须通过 resize 同步存储。
    AudioDataFormat afmt;

    /// @brief 默认构造逻辑空缓冲，不调用对齐分配器。
    AudioBuffer() = default;
    AudioBuffer(const AudioDataFormat& format, size_t num_frames = 0);

    // 禁用拷贝，但实现高效的移动
    AudioBuffer(const AudioBuffer&)            = delete;
    AudioBuffer& operator=(const AudioBuffer&) = delete;
    AudioBuffer(AudioBuffer&& other) noexcept;
    AudioBuffer& operator=(AudioBuffer&& other) noexcept;

    /// @brief 准备连续存储和固定声道跨度，逻辑短块请改用 set_active_frames。
    /// @warning 会分配并重建地址表，只允许控制线程在处理停止时调用。
    inline void resize(const AudioDataFormat& format, size_t num_frames)
    {
        // 相同格式和逻辑长度直接复用，不清除原有 PCM 内容。
        if ( format == afmt && _original_num_frames == num_frames ) return;
        afmt                 = format;
        _original_num_frames = num_frames;

        // 空格式或零帧使逻辑容量归零，旧声道地址表也必须同步清除。
        if ( afmt.channels == 0 || num_frames == 0 ) {
            _contiguous_buffer.clear();
            channel_pointers_.clear();
            _aligned_num_frames         = 0;
            _storage_aligned_num_frames = 0;
            _frame_capacity             = 0;
            return;
        }

        // 声道存储按向量宽度向上取整，保证下一个声道起点仍满足对齐。
        _aligned_num_frames =
            (num_frames + SIMD_VECTOR_SIZE - 1) & ~(SIMD_VECTOR_SIZE - 1);
        // 存储跨度在此次准备后固定，不随以后 set_active_frames 的短块改变。
        _storage_aligned_num_frames = _aligned_num_frames;
        // 允许激活的上限仍是请求值，额外对齐空间不扩大公开容量契约。
        _frame_capacity = num_frames;

        // 声道共用一次样本分配；容量改变会使此前借出的样本地址失效。
        // 一次性分配所有内存
        const size_t total_floats = afmt.channels * _storage_aligned_num_frames;
        _contiguous_buffer.resize(total_floats);

        // 更新内部指针
        sync_pointers();
    }

    /// @brief 在不改变已分配存储的前提下设置逻辑帧数。
    /// @param numFrames 新逻辑帧数。
    /// @return 未超过预分配容量时返回 true。
    /// @warning 音频回调热路径：本函数不分配内存，也不刷新声道指针。
    inline bool set_active_frames(size_t numFrames)
    {
        // 只调整活动范围；声道实际地址仍由准备阶段的固定跨度决定。
        if ( numFrames > _frame_capacity ) return false;
        // 新块可能需要更大的对齐尾部，但不允许越过既有的逻辑容量上限。
        _original_num_frames = numFrames;
        _aligned_num_frames =
            (numFrames + SIMD_VECTOR_SIZE - 1) & ~(SIMD_VECTOR_SIZE - 1);
        return true;
    }

    /// @brief 获取当前存储能够容纳的最大帧数。
    /// @return 每声道最大帧数。
    [[nodiscard]] inline size_t frame_capacity() const
    {
        return _frame_capacity;
    }

    /// @brief 连同对齐填充区一起清零，避免向量混音读取旧尾部值。
    /// @warning 音频热路径只清理既有容量，不更改地址表。
    inline void clear()
    {
        if ( !_contiguous_buffer.empty() ) {
            // 对于浮点数0,memset是安全且通常最快的
            std::memset(_contiguous_buffer.data(),
                        0,
                        _contiguous_buffer.size() * sizeof(float));
        }
    }
    /// @brief 清除有效块的尾部，不触碰此前已经写入的前缀 PCM。
    /// @warning 每音频块可调用，不分配且不修改声道地址表。
    void clear_from(size_t start_frame)
    {
        if ( start_frame >= _original_num_frames ) {
            return;
        }

        // 需要清零的帧数
        const size_t frames_to_clear = _original_num_frames - start_frame;
        if ( frames_to_clear == 0 ) return;

        // 逐声道清零
        for ( uint16_t ch = 0; ch < afmt.channels; ++ch ) {
            // 获取第 ch 声道的起始指针
            float* channel_start_ptr = channel_pointers_[ch];

            // 计算需要清零的内存区域的起始地址
            float* clear_start_ptr = channel_start_ptr + start_frame;

            // 调用 memset
            std::memset(clear_start_ptr, 0, frames_to_clear * sizeof(float));
        }
    }

    /// @brief 借用可写声道表，容量改变或销毁后原地址不再有效。
    inline float** raw_ptrs()
    {
        return channel_pointers_.empty() ? nullptr : channel_pointers_.data();
    }
    /// @brief 只读借用样本和声道地址，不延长缓冲生命周期。
    inline const float* const* raw_ptrs() const
    {
        return channel_pointers_.empty()
                   ? nullptr
                   : reinterpret_cast<const float* const*>(
                         channel_pointers_.data());
    }

    /// @brief 当前有效帧数，交给非向量消费者时不包含对齐尾部。
    inline size_t num_frames() const { return _original_num_frames; }
    /// @brief 当前可索引的声道数，不由有效帧数推导。
    inline uint16_t num_channels() const { return afmt.channels; }
    /// @brief 当前块用于向量处理的补齐长度，可能超过逻辑帧数。
    inline size_t aligned_frames_per_channel() const
    {
        return _aligned_num_frames;
    }

    /// @brief 将双声道交错 PCM 按标量方式拆到两个平面。
    /// 调用方保证至少两个目标声道，源可读且目标容量覆盖 num_frames。
    /// @warning 音频热路径不检查容量，不能传入超过预准备长度的请求。
    inline void write_interleaved_naive(const float* src, size_t num_frames)
    {
        float** dest_channels = this->raw_ptrs();
        float*  dest_L        = dest_channels[0];
        float*  dest_R        = dest_channels[1];
        for ( size_t i = 0; i < num_frames; ++i ) {
            dest_L[i] = src[i * 2 + 0];  // 写入左声道
            dest_R[i] = src[i * 2 + 1];  // 写入右声道
        }
    }
    /// @brief 历史 SIMD 双声道拆分入口，按对齐长度访问输入。
    /// @warning 当前四帧步长与八浮点写入不匹配，不能视为已验证安全的转换。
    /// 调用前需独立审查尾部容量、写入对齐及重排结果，普通块长不足以保证安全。
    inline void write_interleaved_stereo(const float* src)
    {
        // 假设 afmt.channels == 2 且缓冲区大小匹配
        float**      dest           = this->raw_ptrs();
        const size_t aligned_frames = this->aligned_frames_per_channel();

        // 我们一次处理8个浮点数（4帧），所以循环步长为4
        for ( size_t i = 0; i < aligned_frames; i += 4 ) {
            // 加载4个交错的立体声样本 (L0,R0,L1,R1,L2,R2,L3,R3)
            // 此处实际使用一次 256 位非对齐加载读取四个交错立体声帧。
            __m256 interleaved_vec = _mm256_loadu_ps(
                src + i * 2);  // 使用 unaligned load，因为源不保证对齐

            // 解交错
            // 目标:
            //   - 一个寄存器包含 [L0, L1, L2, L3, ?, ?, ?, ?]
            //   - 另一个寄存器包含 [R0, R1, R2, R3, ?, ?, ?, ?]
            //
            // 使用 _mm256_shuffle_ps, 控制码 0b11011000 (0xD8)
            // 它将源向量的元素按 [2,0,3,1]
            // 的模式重排，分别在低128位和高128位通道内 [L0,R0,L1,R1,
            // L2,R2,L3,R3] -> [L0,L1,R0,R1, L2,L3,R2,R3]
            __m256 shuffled =
                _mm256_shuffle_ps(interleaved_vec, interleaved_vec, 0xD8);

            // 历史跨 128 位 lane 重排；不能仅凭变量名认定已得到连续左声道。
            // 该入口的正确性必须结合前一步 shuffle 与最终写入区间一起验证。
            __m256 left_vec = _mm256_permute2f128_ps(shuffled, shuffled, 0x20);

            // 另一控制码选取不同 lane，但仍不能据此认定完整解交错已完成。
            __m256 right_vec = _mm256_permute2f128_ps(shuffled, shuffled, 0x31);

            // 八浮点对齐写入与四帧循环步长存在冲突，保留为独立修复事项。
            _mm256_store_ps(dest[0] + i, left_vec);
            _mm256_store_ps(dest[1] + i, right_vec);
        }
    }

    /// @brief 向量化累加兼容缓冲，包含本块对齐填充区。
    /// @warning 热路径须预先保证格式匹配和源目标不重叠，错误分支沿用异常。
    inline void operator+=(const AudioBuffer& other)
    {
        if ( afmt != other.afmt || num_frames() != other.num_frames() ) {
            throw ice::buffer_error("AudioBuffer format mismatch for mixing.");
        }

        // 使用 ICE_RESTRICT 告知编译器指针不重叠，允许更激进的优化
        float* const* ICE_RESTRICT       all_dest_channels = this->raw_ptrs();
        const float* const* ICE_RESTRICT all_src_channels  = other.raw_ptrs();

        // 向量累加会消费对齐尾部，输入准备流程应保证填充区不含旧有效音频。
        const size_t   aligned_frames = this->aligned_frames_per_channel();
        const uint16_t channels       = this->num_channels();

        // 按声道逐段处理，各声道起点已由准备阶段保证满足向量对齐。
        for ( uint16_t ch = 0; ch < channels; ++ch ) {
            float* ICE_RESTRICT       dest = all_dest_channels[ch];
            const float* ICE_RESTRICT src  = all_src_channels[ch];

            for ( size_t i = 0; i < aligned_frames; i += SIMD_VECTOR_SIZE ) {
                __m256 dest_vec   = _mm256_load_ps(dest + i);
                __m256 src_vec    = _mm256_load_ps(src + i);
                __m256 result_vec = _mm256_add_ps(dest_vec, src_vec);
                _mm256_store_ps(dest + i, result_vec);
            }
        }
    }

private:
    /// @brief 对齐连续存储类型，整个声道集合只持有一个样本分配块。
    using AlignedFloatVector =
        std::vector<float, AlignedAllocator<float, SIMD_ALIGNMENT> >;
    /// @brief 按固定存储跨度建立的借用地址表，本身不拥有样本。
    std::vector<float*> channel_pointers_;

    /// @brief 唯一拥有全部声道及其填充区的对齐连续存储。
    AlignedFloatVector _contiguous_buffer;

    /// @brief 对外可见的每声道有效帧数，不含对齐填充。
    size_t _original_num_frames = 0;
    /// @brief 当前块的向量化访问长度，不等同于固定声道存储跨度。
    size_t _aligned_num_frames = 0;

    /// @brief 每声道实际存储跨度，逻辑帧数变化时保持不变。
    size_t _storage_aligned_num_frames = 0;

    /// @brief 每声道能够激活的最大帧数。
    size_t _frame_capacity = 0;

    /// @brief 根据固定存储跨度重建各声道地址。
    /// @warning 指针表可扩容，只允许在存储准备阶段调用。
    inline void sync_pointers()
    {
        // 零声道不允许发布任何可索引的样本指针。
        if ( afmt.channels == 0 ) {
            channel_pointers_.clear();
            return;
        }

        // 地址表准备与样本准备同属控制阶段，逐块切换长度不经过此处。
        channel_pointers_.resize(afmt.channels);
        float* base_ptr = _contiguous_buffer.data();
        for ( uint16_t i = 0; i < afmt.channels; ++i ) {
            // 指针指向连续内存块的正确偏移位置
            channel_pointers_[i] = base_ptr + i * _storage_aligned_num_frames;
        }
    }
};
#endif  // __linux__ 平台存储分支
}  // namespace ice

#endif  // ICE_AUDIOBUFFER_HPP
