#ifndef ICE_AUDIOBUFFER_HPP
#define ICE_AUDIOBUFFER_HPP

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <ice/execptions/buffer_error.hpp>
#include <ice/manage/AudioFormat.hpp>
#include <vector>
#ifdef __APPLE__
#    include <algorithm>
#endif  //__APPLE__
namespace ice
{
#ifndef __linux__
class AudioBuffer
{
public:
    // --- Public 接口与你的优化版本完全一致 ---
    AudioDataFormat afmt;
    // 构造与生命周期
    AudioBuffer() = default;
    AudioBuffer(const AudioDataFormat& format, size_t num_frames = 0);

    // 删除拷贝，实现移动
    AudioBuffer(const AudioBuffer&)            = delete;
    AudioBuffer& operator=(const AudioBuffer&) = delete;
    AudioBuffer(AudioBuffer&& other) noexcept;
    AudioBuffer& operator=(AudioBuffer&& other) noexcept;

    // 核心功能
    void resize(const AudioDataFormat& format, size_t num_frames);
    void clear()
    {
        for ( auto& channel_data : _data )
            {
                std::fill(channel_data.begin(), channel_data.end(), 0.0f);
            }
    }
    void clear_from(size_t start_frame)
    {
        for ( auto& channel_data : _data )
            {
                std::fill(channel_data.begin() + start_frame,
                          channel_data.end(),
                          0.0f);
            }
    }
    // 数据访问 (提供与优化版本相同的接口)
    float** raw_ptrs()
    {
        return channel_pointers_.empty() ? nullptr : channel_pointers_.data();
    }
    const float* const* raw_ptrs() const
    {
        return channel_pointers_.empty()
                   ? nullptr
                   : reinterpret_cast<const float* const*>(
                         channel_pointers_.data());
    }
    // 信息查询
    size_t num_frames() const { return _data.empty() ? 0 : _data[0].size(); }
    size_t num_channels() const { return afmt.channels; }
    // 核心操作：使用纯标量循环
    void operator+=(const AudioBuffer& other)
    {
        if ( afmt != other.afmt || num_frames() != other.num_frames() )
            {
                throw ice::buffer_error(
                    "AudioBuffer_Baseline format mismatch for mixing.");
            }
        const size_t   frames   = num_frames();
        const uint16_t channels = num_channels();
        for ( uint16_t ch = 0; ch < channels; ++ch )
            {
                float*       dest = this->raw_ptrs()[ch];
                const float* src  = other.raw_ptrs()[ch];
                // **基准实现：纯粹的、未优化的标量循环**
                for ( size_t i = 0; i < frames; ++i )
                    {
                        dest[i] += src[i];
                    }
            }
    }

private:
    // **内部实现：朴素的 SoA (结构数组)**
    // 使用默认分配器，不保证对齐
    using ChannelData = std::vector<float>;
    // // 1. 每个声道都是一个独立的std::vector，内存不连续
    std::vector<ChannelData> _data;
    // 2. 依然需要指针数组以匹配接口
    std::vector<float*> channel_pointers_;
    // 内部辅助函数
    void sync_pointers();
};
#else
// #if defined(_MSC_VER)
// #include <malloc.h>
// // 适用于 Microsoft Visual C++
// #define ICE_RESTRICT __declspec(restrict)
// #else
// // 适用于 GCC 和 Clang
#    define ICE_RESTRICT __restrict__
#    include <mm_malloc.h>
// #endif

// 分配器
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

    template<typename U> struct rebind
    {
        using other = AlignedAllocator<U, Alignment>;
    };

    AlignedAllocator() noexcept {}
    template<typename U>
    AlignedAllocator(const AlignedAllocator<U, Alignment>&) noexcept
    {
    }

    pointer allocate(size_type n)
    {
        if ( n > std::size_t(-1) / sizeof(T) )
            {
                throw std::bad_alloc();
            }
        if ( auto p =
                 static_cast<pointer>(_mm_malloc(n * sizeof(T), Alignment)) )
            {
                return p;
            }
        throw std::bad_alloc();
    }

    void deallocate(pointer p, size_type) noexcept { _mm_free(p); }

    // 这些是C++11及以后标准所必需的
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

// 包含所有SIMD指令集的头文件
#    include <immintrin.h>

class AudioBuffer
{
public:
    // For AVX2
    static constexpr size_t SIMD_ALIGNMENT = 32;
    // 8 floats in a __m256 register
    static constexpr size_t SIMD_VECTOR_SIZE = 8;

    AudioDataFormat afmt;

    // 构造与生命周期管理
    AudioBuffer() = default;
    AudioBuffer(const AudioDataFormat& format, size_t num_frames = 0);

    // 禁用拷贝，但实现高效的移动
    AudioBuffer(const AudioBuffer&)            = delete;
    AudioBuffer& operator=(const AudioBuffer&) = delete;
    AudioBuffer(AudioBuffer&& other) noexcept;
    AudioBuffer& operator=(AudioBuffer&& other) noexcept;

    inline void resize(const AudioDataFormat& format, size_t num_frames)
    {
        if ( format == afmt && _original_num_frames == num_frames ) return;
        afmt                 = format;
        _original_num_frames = num_frames;

        if ( afmt.channels == 0 || num_frames == 0 )
            {
                _contiguous_buffer.clear();
                channel_pointers_.clear();
                _aligned_num_frames = 0;
                return;
            }

        // 同样，计算对齐后的帧数
        _aligned_num_frames =
            (num_frames + SIMD_VECTOR_SIZE - 1) & ~(SIMD_VECTOR_SIZE - 1);

        // 一次性分配所有内存
        const size_t total_floats = afmt.channels * _aligned_num_frames;
        _contiguous_buffer.resize(total_floats);

        // 更新内部指针
        sync_pointers();
    }

    inline void clear()
    {
        if ( !_contiguous_buffer.empty() )
            {
                // 对于浮点数0,memset是安全且通常最快的
                std::memset(_contiguous_buffer.data(),
                            0,
                            _contiguous_buffer.size() * sizeof(float));
            }
    }
    void clear_from(size_t start_frame)
    {
        if ( start_frame >= _original_num_frames )
            {
                return;
            }

        // 需要清零的帧数
        const size_t frames_to_clear = _original_num_frames - start_frame;
        if ( frames_to_clear == 0 ) return;

        // 逐声道清零
        for ( uint16_t ch = 0; ch < afmt.channels; ++ch )
            {
                // 获取第 ch 声道的起始指针
                float* channel_start_ptr = channel_pointers_[ch];

                // 计算需要清零的内存区域的起始地址
                float* clear_start_ptr = channel_start_ptr + start_frame;

                // 调用 memset
                std::memset(
                    clear_start_ptr, 0, frames_to_clear * sizeof(float));
            }
    }

    // 数据访问
    inline float** raw_ptrs()
    {
        return channel_pointers_.empty() ? nullptr : channel_pointers_.data();
    }
    inline const float* const* raw_ptrs() const
    {
        return channel_pointers_.empty()
                   ? nullptr
                   : reinterpret_cast<const float* const*>(
                         channel_pointers_.data());
    }

    inline size_t   num_frames() const { return _original_num_frames; }
    inline uint16_t num_channels() const { return afmt.channels; }
    inline size_t   aligned_frames_per_channel() const
    {
        return _aligned_num_frames;
    }

    // 假设 src 是 [L0, R0, L1, R1, ...]
    // dest_channels 是 this->raw_ptrs()
    inline void write_interleaved_naive(const float* src, size_t num_frames)
    {
        float** dest_channels = this->raw_ptrs();
        float*  dest_L        = dest_channels[0];
        float*  dest_R        = dest_channels[1];
        for ( size_t i = 0; i < num_frames; ++i )
            {
                dest_L[i] = src[i * 2 + 0];  // 写入左声道
                dest_R[i] = src[i * 2 + 1];  // 写入右声道
            }
    }
    inline void write_interleaved_stereo(const float* src)
    {
        // 假设 afmt.channels == 2 且缓冲区大小匹配
        float**      dest           = this->raw_ptrs();
        const size_t aligned_frames = this->aligned_frames_per_channel();

        // 我们一次处理8个浮点数（4帧），所以循环步长为4
        for ( size_t i = 0; i < aligned_frames; i += 4 )
            {
                // 加载4个交错的立体声样本 (L0,R0,L1,R1,L2,R2,L3,R3)
                // 我们一次加载两个 __m128 来组成一个 __m256
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

                // 使用 _mm256_permute2f128_ps 跨128位通道重排
                // 控制码 0b00100000 (0x20)
                // 它将 shuffled 的低128位和高128位组合成最终的左声道向量
                // [L0,L1,R0,R1], [L2,L3,R2,R3] -> [L0,L1,L2,L3]
                __m256 left_vec =
                    _mm256_permute2f128_ps(shuffled, shuffled, 0x20);

                // 控制码 0b00110001 (0x31)
                // 它将 shuffled 的高128位和低128位组合成最终的右声道向量
                // [L0,L1,R0,R1], [L2,L3,R2,R3] -> [R0,R1,R2,R3]
                __m256 right_vec =
                    _mm256_permute2f128_ps(shuffled, shuffled, 0x31);

                // 将解交错后的向量写入各自的对齐内存中
                _mm256_store_ps(dest[0] + i, left_vec);
                _mm256_store_ps(dest[1] + i, right_vec);
            }
    }

    // 核心操作
    inline void operator+=(const AudioBuffer& other)
    {
        if ( afmt != other.afmt || num_frames() != other.num_frames() )
            {
                throw ice::buffer_error(
                    "AudioBuffer format mismatch for mixing.");
            }

        // 使用 ICE_RESTRICT 告知编译器指针不重叠，允许更激进的优化
        float* const* ICE_RESTRICT       all_dest_channels = this->raw_ptrs();
        const float* const* ICE_RESTRICT all_src_channels  = other.raw_ptrs();

        const size_t   aligned_frames = this->aligned_frames_per_channel();
        const uint16_t channels       = this->num_channels();

        // 循环结构保持不变，但现在它操作在连续内存上，缓存效率极高
        for ( uint16_t ch = 0; ch < channels; ++ch )
            {
                float* ICE_RESTRICT       dest = all_dest_channels[ch];
                const float* ICE_RESTRICT src  = all_src_channels[ch];

                for ( size_t i = 0; i < aligned_frames; i += SIMD_VECTOR_SIZE )
                    {
                        __m256 dest_vec   = _mm256_load_ps(dest + i);
                        __m256 src_vec    = _mm256_load_ps(src + i);
                        __m256 result_vec = _mm256_add_ps(dest_vec, src_vec);
                        _mm256_store_ps(dest + i, result_vec);
                    }
            }
    }

private:
    // 使用 using 提高可读性
    using AlignedFloatVector =
        std::vector<float, AlignedAllocator<float, SIMD_ALIGNMENT> >;
    // 指向连续内存块中各个声道的起始位置
    std::vector<float*> channel_pointers_;

    // 单一连续内存块
    AlignedFloatVector _contiguous_buffer;

    size_t _original_num_frames = 0;
    size_t _aligned_num_frames  = 0;

    // 内部辅助函数，用于根据 _contiguous_buffer 更新指针
    inline void sync_pointers()
    {
        if ( afmt.channels == 0 )
            {
                channel_pointers_.clear();
                return;
            }

        channel_pointers_.resize(afmt.channels);
        float* base_ptr = _contiguous_buffer.data();
        for ( uint16_t i = 0; i < afmt.channels; ++i )
            {
                // 指针指向连续内存块的正确偏移位置
                channel_pointers_[i] = base_ptr + i * _aligned_num_frames;
            }
    }
};
#endif  //__APPLE__
}  // namespace ice

#endif  // ICE_AUDIOBUFFER_HPP
