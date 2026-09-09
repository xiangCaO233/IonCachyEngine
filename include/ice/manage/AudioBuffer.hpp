#pragma once

#include <ice/manage/AudioFormat.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

#ifdef __linux__
// 系统头必须位于全局作用域，避免其声明随业务头的包含顺序进入 ice。
// SIMD 实现依赖编译目标的 AVX 能力，不在本头做运行时 CPU 派发。
#    include <immintrin.h>
#    include <mm_malloc.h>
#endif

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
    /// @brief 按格式准备存储，借用样本前需确认活动帧数和声道数。
    AudioBuffer(const AudioDataFormat& format, size_t num_frames = 0);

    /// @brief 禁止隐式复制整块 PCM 存储。
    AudioBuffer(const AudioBuffer&) = delete;
    /// @brief 禁止复制赋值，避免隐藏的存储分配与覆盖。
    AudioBuffer& operator=(const AudioBuffer&) = delete;
    /// @brief 转移存储及声道表，源的声道数、活动长度与容量清零。
    AudioBuffer(AudioBuffer&& other) noexcept;
    /// @brief 接管所有权并释放目标旧存储，自移动保持原状态。
    /// @warning 目标旧样本借用失效；可能回收内存，仅用于控制阶段。
    AudioBuffer& operator=(AudioBuffer&& other) noexcept;

    /// @brief 控制阶段调整存储大小，可能使所有旧借用地址失效。
    /// @return 尺寸可表示且调整完成时为 true；尺寸超界返回 false 并保留原状态。
    /// @details 零声道统一清空活动长度与容量，无论请求帧数是否为零。
    /// 扩容先准备替代存储，分配失败保留原状态，但仍可能抛出异常。
    bool resize(const AudioDataFormat& format, size_t num_frames);

    /// @brief 在不改变已分配存储的前提下设置逻辑帧数。
    /// @param numFrames 新逻辑帧数。
    /// @return 未超过预分配容量时返回 true。
    /// @warning 音频回调热路径：本函数不分配内存。
    /// 扩大活动长度不会清除重新暴露的旧样本，调用方须在消费前填充或清零。
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
    /// @return 格式或活动长度不匹配时返回 false，目标保持不变。
    /// @warning 每个混音块调用，禁止引入分配、阻塞或异常。
    bool operator+=(const AudioBuffer& other) noexcept
    {
        if ( afmt != other.afmt || num_frames() != other.num_frames() ) {
            // 验证先于样本访问，拒绝整块而不产生部分混音结果。
            return false;
        }
        // 默认构造的空缓冲没有声道表；兼容的零帧混音无需访问存储。
        // 保留前置格式校验，使空输入也遵守整块兼容性契约。
        if ( num_frames() == 0U ) return true;
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
        return true;
    }

private:
    /// @brief 单声道存储，不承诺 SIMD 所需的额外地址对齐。
    using ChannelData = std::vector<float>;
    /// @brief 声道之间不保证相邻，每个声道内部连续。
    std::vector<ChannelData> _data;
    /// @brief 非拥有的平面地址表，调用方不得改写表项使其偏离自有存储。
    std::vector<float*> channel_pointers_;

    /// @brief 当前对外可见的逻辑帧数。
    size_t m_activeFrames{ 0U };

    /// @brief 每声道已经分配的最大帧数。
    size_t m_frameCapacity{ 0U };

    /// @brief 存储变化后刷新各声道借用地址，调用可能使地址表扩容。
    void sync_pointers();
};
#else
/// @brief Linux 连续平面 PCM 缓冲，声道起点按固定 SIMD 跨度排列。
/// 有效长度可逐块改变，但已有存储的声道间隔只在 resize 时更新。
class AudioBuffer
{
    /// @brief 独占连续样本存储，以返回值传播对齐申请失败。
    /// 仅用于平凡 float 元素，保留原 vector 的前缀和增长清零语义。
    class AlignedSampleStorage
    {
    public:
        /// @brief 空存储不分配，删除器始终与对齐申请入口配对。
        AlignedSampleStorage() = default;
        /// @brief 转移存储后将源的长度和容量归零，使其可继续复用。
        AlignedSampleStorage(AlignedSampleStorage&& other) noexcept
            : m_data(std::move(other.m_data))
            , m_size(std::exchange(other.m_size, 0))
            , m_capacity(std::exchange(other.m_capacity, 0))
        {
        }
        /// @brief 接管存储并释放旧块，自移动保持已有样本。
        AlignedSampleStorage& operator=(AlignedSampleStorage&& other) noexcept
        {
            if ( this != &other ) {
                m_data     = std::move(other.m_data);
                m_size     = std::exchange(other.m_size, 0);
                m_capacity = std::exchange(other.m_capacity, 0);
            }
            return *this;
        }
        /// @brief 限制可表示的样本数，防止字节乘法和指针差值溢出。
        static size_t max_size() noexcept
        {
            return static_cast<size_t>(
                       std::numeric_limits<std::ptrdiff_t>::max()) /
                   sizeof(float);
        }
        /// @brief 调整有效元素数；扩容失败时原地址、容量和样本均保持不变。
        /// @warning 控制侧分配和释放；热路径不得调整存储容量。
        bool resize(size_t count)
        {
            if ( count > max_size() ) return false;
            if ( count > m_capacity ) {
                // 先准备新块，失败不提交任何可见状态，也不释放旧存储。
                std::unique_ptr<float, decltype(&_mm_free)> prepared(
                    static_cast<float*>(
                        _mm_malloc(count * sizeof(float), SIMD_ALIGNMENT)),
                    &_mm_free);
                if ( !prepared ) return false;
                // 连续前缀保持原顺序，声道跨度解释仍由外层 resize 决定。
                if ( m_size != 0 )
                    std::memcpy(
                        prepared.get(), m_data.get(), m_size * sizeof(float));
                std::memset(prepared.get() + m_size,
                            0,
                            (count - m_size) * sizeof(float));
                m_data     = std::move(prepared);
                m_capacity = count;
            } else if ( count > m_size ) {
                // 缩短后重新增长也必须清零新暴露区域，不能恢复先前的旧尾部。
                std::memset(
                    m_data.get() + m_size, 0, (count - m_size) * sizeof(float));
            }
            m_size = count;
            return true;
        }
        /// @brief 清除有效元素，保留容量以支持低频准备复用。
        void clear() noexcept { m_size = 0; }
        /// @brief 查询有效元素是否为空，不以保留容量判断。
        bool empty() const noexcept { return m_size == 0; }
        /// @brief 返回有效元素数，不包括尚未激活的保留容量。
        size_t size() const noexcept { return m_size; }
        /// @brief 借用当前对齐样本地址，扩容或移动赋值后须重新获取。
        float* data() noexcept { return m_data.get(); }

    private:
        /// @brief 唯一拥有对齐块，析构通过匹配的释放入口回收。
        std::unique_ptr<float, decltype(&_mm_free)> m_data{ nullptr,
                                                            &_mm_free };
        /// @brief 已初始化的有效样本数，缩短时不释放容量。
        size_t m_size{ 0 };
        /// @brief 当前块最多能容纳的样本数，失败分配不修改。
        size_t m_capacity{ 0 };
    };

public:
    /// @brief 连续存储的字节对齐要求。
    static constexpr size_t SIMD_ALIGNMENT = 32;
    /// @brief 每个向量容纳的浮点采样数，用于向上取整声道跨度。
    static constexpr size_t SIMD_VECTOR_SIZE = 8;

    /// @brief 样本格式快照，声道布局变更必须通过 resize 同步存储。
    AudioDataFormat afmt;

    /// @brief 默认构造逻辑空缓冲，不调用对齐分配器。
    AudioBuffer() = default;
    /// @brief 按格式准备存储，借用样本前需确认活动帧数和声道数。
    AudioBuffer(const AudioDataFormat& format, size_t num_frames = 0);

    /// @brief 禁止隐式复制整块 PCM 存储。
    AudioBuffer(const AudioBuffer&) = delete;
    /// @brief 禁止复制赋值，避免隐藏的存储分配与覆盖。
    AudioBuffer& operator=(const AudioBuffer&) = delete;
    /// @brief 转移存储及声道表，源的声道数、活动长度与容量清零。
    AudioBuffer(AudioBuffer&& other) noexcept;
    /// @brief 接管所有权并释放目标旧存储，自移动保持原状态。
    /// @warning 目标旧样本借用失效；可能回收内存，仅用于控制阶段。
    AudioBuffer& operator=(AudioBuffer&& other) noexcept;

    /// @brief 准备连续存储和固定声道跨度，逻辑短块请改用 set_active_frames。
    /// @warning 会分配并重建地址表，只允许控制线程在处理停止时调用。
    /// @return 尺寸超界时为 false，原格式、容量和样本保持不变。
    /// @details 零声道将请求帧数归零，建立无可激活样本的空状态。
    /// 样本与声道表申请失败均返回 false，失败前不修改原状态。
    inline bool resize(const AudioDataFormat& format, size_t num_frames)
    {
        // 零声道不描述任何样本，活动长度与容量必须共同归零，不能保留虚假帧数。
        if ( format.channels == 0 ) num_frames = 0;
        // 活动长度可独立缩短，仅它相同不能跳过调用方显式请求的容量调整。
        // 格式、活动长度和准备容量全部一致才复用，仍保留已有 PCM 内容。
        if ( format == afmt && _original_num_frames == num_frames &&
             _frame_capacity == num_frames )
            return true;
        size_t alignedFrames = 0;
        if ( format.channels != 0 && num_frames != 0 ) {
            // 对齐加法先校验，不能让最大尺寸绕回零而发布虚假的活动长度。
            if ( num_frames >
                 std::numeric_limits<size_t>::max() - (SIMD_VECTOR_SIZE - 1) )
                return false;
            alignedFrames =
                (num_frames + SIMD_VECTOR_SIZE - 1) & ~(SIMD_VECTOR_SIZE - 1);
            // 用除法检查总元素数，同时遵守存储的元素/字节容量上限。
            if ( alignedFrames >
                 _contiguous_buffer.max_size() / format.channels )
                return false;
        }
        // 空状态不需要分配，可直接同步清除格式、活动范围与声道表。
        if ( format.channels == 0 || num_frames == 0 ) {
            afmt                 = format;
            _original_num_frames = 0;
            _contiguous_buffer.clear();
            // 空状态保留表容量，raw_ptrs 根据准备容量隐藏旧表项。
            _aligned_num_frames         = 0;
            _storage_aligned_num_frames = 0;
            _frame_capacity             = 0;
            return true;
        }

        // 扩容指针表先在临时容器中完成，失败不会使旧表的借用地址失效。
        // 表容量足够时不额外分配；样本调整成功后刷新表也不会再扩容。
        std::unique_ptr<float*, decltype(&std::free)> preparedPointers{
            nullptr, &std::free
        };
        if ( format.channels > m_pointerCapacity ) {
            // 声道数为 uint16_t，转换为 size_t 后乘指针宽度不会溢出。
            preparedPointers.reset(static_cast<float**>(std::malloc(
                static_cast<size_t>(format.channels) * sizeof(float*))));
            if ( !preparedPointers ) return false;
        }
        // float 元素调整失败保持原存储，此前不能修改格式或公开容量。
        // 沿用连续存储的前缀保留语义，不在本次失败修复中改变声道重排规则。
        const size_t total_floats = format.channels * alignedFrames;
        if ( !_contiguous_buffer.resize(total_floats) ) return false;

        // 此后仅转移已准备的表并更新元数据，不再发生新的存储申请。
        if ( preparedPointers ) {
            channel_pointers_ = std::move(preparedPointers);
            m_pointerCapacity = format.channels;
        }
        afmt                        = format;
        _original_num_frames        = num_frames;
        _aligned_num_frames         = alignedFrames;
        _storage_aligned_num_frames = alignedFrames;
        // 对齐填充不扩大允许激活的逻辑容量，声道跨度仍固定到本次准备结果。
        _frame_capacity = num_frames;
        sync_pointers();
        return true;
    }

    /// @brief 在不改变已分配存储的前提下设置逻辑帧数。
    /// @param numFrames 新逻辑帧数。
    /// @return 未超过预分配容量时返回 true。
    /// @warning 音频回调热路径：本函数不分配内存，也不刷新声道指针。
    /// 活动范围和新对齐尾部不自动清零，扩大范围后不能假设其数据为静音。
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
            // 使用目标平台浮点零的全零位表示，一并清理全部声道及其填充区。
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

        // 仅清活动范围，对齐尾部不在此清除；尾部需由完整 clear 或写入流程维护。
        const size_t frames_to_clear = _original_num_frames - start_frame;
        if ( frames_to_clear == 0 ) return;

        // 地址使用固定存储跨度，不能按当前短块长度重新计算声道位置。
        for ( uint16_t ch = 0; ch < afmt.channels; ++ch ) {
            float* channel_start_ptr = channel_pointers_.get()[ch];

            float* clear_start_ptr = channel_start_ptr + start_frame;

            std::memset(clear_start_ptr, 0, frames_to_clear * sizeof(float));
        }
    }

    /// @brief 借用可写声道表，容量改变或销毁后原地址不再有效。
    inline float** raw_ptrs()
    {
        return _frame_capacity == 0 ? nullptr : channel_pointers_.get();
    }
    /// @brief 只读借用样本和声道地址，不延长缓冲生命周期。
    inline const float* const* raw_ptrs() const
    {
        return _frame_capacity == 0 ? nullptr
                                    : reinterpret_cast<const float* const*>(
                                          channel_pointers_.get());
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
    /// @brief 将双声道交错 PCM 拆分到活动范围，不读写对齐尾部。
    /// @param src 至少包含活动帧数两倍的浮点样本，与目标存储不重叠。
    /// 目标活动区之外保持原内容，调用方需要静音填充时应显式清理。
    /// 源指针不要求额外 SIMD 对齐；接口不持有指针，返回后即可释放源。
    /// 固定声道数和活动长度须由准备接口维护，处理期间不得并发修改。
    /// @warning 音频热路径复用固定容量，空指针或非双声道格式保持目标不变。
    inline void write_interleaved_stereo(const float* src)
    {
        // 在索引左右声道前拒绝不适用的格式，零帧不需要有效源地址。
        if ( !src || num_channels() != 2U || num_frames() == 0U ) return;
        float**      dest   = this->raw_ptrs();
        const size_t frames = num_frames();
        size_t       i      = 0U;
        // 每次输入八浮点即四帧，输出每声道恰好四浮点，保持步长与写宽一致。
        // 只处理完整四帧组，不使用向上补齐长度，源无需额外可读填充。
        for ( ; frames - i >= 4U; i += 4U ) {
            const __m128 first  = _mm_loadu_ps(src + i * 2U);
            const __m128 second = _mm_loadu_ps(src + i * 2U + 4U);
            // 两输入各选偶数位置得到 L0..L3，奇数位置得到 R0..R3。
            const __m128 left =
                _mm_shuffle_ps(first, second, _MM_SHUFFLE(2, 0, 2, 0));
            const __m128 right =
                _mm_shuffle_ps(first, second, _MM_SHUFFLE(3, 1, 3, 1));
            // 非对齐存储不依赖每组地址满足原 256 位指令的对齐要求。
            _mm_storeu_ps(dest[0] + i, left);
            _mm_storeu_ps(dest[1] + i, right);
        }
        // 最多三帧的尾部按标量处理，不覆盖下一声道或目标容量中的保留区。
        for ( ; i < frames; ++i ) {
            dest[0][i] = src[i * 2U];
            dest[1][i] = src[i * 2U + 1U];
        }
    }

    /// @brief 向量化累加兼容缓冲的活动前缀，允许自身累加并保留未激活尾部。
    /// @return 格式或活动长度不匹配时返回 false，目标保持不变。
    /// @warning 每个混音块调用，禁止引入分配、阻塞或异常。
    inline bool operator+=(const AudioBuffer& other) noexcept
    {
        if ( afmt != other.afmt || num_frames() != other.num_frames() ) {
            // 验证先于样本访问，拒绝整块而不产生部分混音结果。
            return false;
        }
        // 默认构造的空缓冲没有声道表；兼容的零帧混音无需访问存储。
        // 保留前置格式校验，使空输入也遵守整块兼容性契约。
        if ( num_frames() == 0U ) return true;

        // 自身累加属于有效输入，不能向编译器声明源目标必定不重叠。
        float* const*       all_dest_channels = this->raw_ptrs();
        const float* const* all_src_channels  = other.raw_ptrs();

        // 只处理活动帧，预留空间及旧块尾部不会参与当前混音。
        const size_t   frames   = this->num_frames();
        const uint16_t channels = this->num_channels();

        // 按声道逐段处理，各声道起点已由准备阶段保证满足向量对齐。
        for ( uint16_t ch = 0; ch < channels; ++ch ) {
            float*       dest = all_dest_channels[ch];
            const float* src  = all_src_channels[ch];

            size_t i = 0;
            // 完整向量组仍使用对齐加载，短尾不能跨过活动边界。
            for ( ; frames - i >= SIMD_VECTOR_SIZE; i += SIMD_VECTOR_SIZE ) {
                __m256 dest_vec   = _mm256_load_ps(dest + i);
                __m256 src_vec    = _mm256_load_ps(src + i);
                __m256 result_vec = _mm256_add_ps(dest_vec, src_vec);
                _mm256_store_ps(dest + i, result_vec);
            }
            // 剩余不足一个向量的样本逐个累加，零帧时两条循环均不执行。
            for ( ; i < frames; ++i ) {
                dest[i] += src[i];
            }
        }
        return true;
    }

private:
    /// @brief 按固定存储跨度建立的借用地址表，本身不拥有样本。
    std::unique_ptr<float*, decltype(&std::free)> channel_pointers_{
        nullptr, &std::free
    };
    /// @brief 指针表已准备的元素容量，空状态保留，移动时与表一同转移。
    size_t m_pointerCapacity{ 0 };

    /// @brief 唯一拥有全部声道及其填充区的对齐连续存储。
    AlignedSampleStorage _contiguous_buffer;

    /// @brief 对外可见的每声道有效帧数，不含对齐填充。
    size_t _original_num_frames = 0;
    /// @brief 当前块的向量化访问长度，不等同于固定声道存储跨度。
    size_t _aligned_num_frames = 0;

    /// @brief 每声道实际存储跨度，逻辑帧数变化时保持不变。
    size_t _storage_aligned_num_frames = 0;

    /// @brief 每声道能够激活的最大帧数。
    size_t _frame_capacity = 0;

    /// @brief 根据固定存储跨度重建各声道地址。
    /// @pre resize 已准备足够的指针表容量，此处不分配。
    inline void sync_pointers()
    {
        // 零声道不允许发布任何可索引的样本指针。
        if ( afmt.channels == 0 ) {
            // 空状态保留表容量，raw_ptrs 根据准备容量隐藏旧表项。
            return;
        }

        // 样本和表都已成功准备，只填充借用地址，不能在提交后再次申请内存。
        float* base_ptr = _contiguous_buffer.data();
        for ( uint16_t i = 0; i < afmt.channels; ++i ) {
            // 指针指向连续内存块的正确偏移位置
            channel_pointers_.get()[i] =
                base_ptr + i * _storage_aligned_num_frames;
        }
    }
};
#endif  // __linux__ 平台存储分支
}  // namespace ice
