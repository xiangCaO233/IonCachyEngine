#ifndef ICE_MIXBUS_HPP
#define ICE_MIXBUS_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include "ice/core/IAudioNode.hpp"
#include "ice/manage/AudioBuffer.hpp"

namespace ice
{
/// @brief 混音总线的双声道输出模式。
enum class MixBusChannelMode : uint8_t {
    Stereo = 0,       ///< 保持原始立体声输出。
    MuteLeft,         ///< 静音左声道，仅保留右声道。
    MuteRight,        ///< 静音右声道，仅保留左声道。
    CopyLeftToRight,  ///< 将左声道复制到右声道，两侧都播放左声道。
    CopyRightToLeft   ///< 将右声道复制到左声道，两侧都播放右声道。
};

/// @brief 混音总线，通过不可变来源快照汇总多个音频节点。
class MixBus : public IAudioNode
{
public:
    /// @brief 构造混音总线，并按引擎默认格式预备临时缓冲区。
    MixBus();

    /// @brief 析构混音总线。
    /// @warning 销毁前必须停止调用 process 的音频线程。
    ~MixBus() override;

    /// @brief 预备音频回调需要的临时缓冲区。
    /// @param format 回调使用的音频格式。
    /// @param maxFrames 单次无分块处理的最大帧数。
    /// @warning 该函数会分配内存，只能在音频回调停止时由控制线程调用。
    void prepare(const AudioDataFormat& format, std::size_t maxFrames);

    /// @brief 处理并混合当前 block 起点取得的来源快照。
    /// @param buffer 输出缓冲区。
    /// @warning 音频回调热路径：每个音频缓冲周期执行；不得引入锁、内存分配、
    /// shared_ptr 复制或阻塞操作；同一 MixBus
    /// 只允许一个音频线程调用且不可重入。
    void process(AudioBuffer& buffer) override;

    /// @brief 按插入顺序添加输入音频节点。
    /// @param src 输入音频节点；空节点会被忽略。
    void add_source(std::shared_ptr<IAudioNode> src);

    /// @brief 移除输入音频节点。
    /// @param src 输入音频节点。
    void remove_source(const std::shared_ptr<IAudioNode>& src);

    /// @brief 在不改变来源索引的前提下原子替换一个输入节点。
    /// @param current 当前输入节点。
    /// @param replacement 替换节点。
    /// @return 当前节点存在且替换成功时返回 true。
    /// @warning
    /// 控制线程路径：会分配不可变快照并获取控制锁，不得在音频回调调用。
    bool replace_source(const std::shared_ptr<IAudioNode>& current,
                        std::shared_ptr<IAudioNode>        replacement);

    /// @brief 清空全部输入音频节点。
    void clear();

    /// @brief 在控制线程回收已越过音频读取临界区的来源快照。
    /// @warning 该函数可能释放内存并获取控制锁，不得在音频回调调用。
    void reclaimRetiredSources();

    /// @brief 获取当前控制侧来源数量。
    /// @return 当前来源数量。
    /// @warning 该函数会获取控制锁，不得在音频回调调用。
    std::size_t sourceCount() const;

    /// @brief 获取尚待安全回收的历史快照数量。
    /// @return 历史快照数量。
    /// @warning 该函数会获取控制锁，不得在音频回调调用。
    std::size_t retiredSnapshotCount() const;

    /// @brief 获取当前预备的最大帧数。
    /// @return 最大帧数；未预备时返回零。
    /// @warning prepare 不得与该函数并发调用。
    std::size_t maxPreparedFrames() const;

    /// @brief 获取因超过预备帧数而采用分块处理的 block 次数。
    /// @return 分块处理次数。
    std::uint64_t oversizedProcessCount() const;

    /// @brief 获取因未预备或格式不匹配而输出静音的 block 次数。
    /// @return 被拒绝的处理次数。
    std::uint64_t rejectedProcessCount() const;

    /// @brief 设置双声道输出模式。
    /// @param mode 目标输出模式。
    void set_channel_mode(MixBusChannelMode mode)
    {
        m_channelMode.store(static_cast<uint8_t>(mode),
                            std::memory_order_relaxed);
    }

    /// @brief 获取当前双声道输出模式。
    /// @return 当前输出模式。
    MixBusChannelMode get_channel_mode() const
    {
        return static_cast<MixBusChannelMode>(
            m_channelMode.load(std::memory_order_relaxed));
    }

    /// @brief 兼容旧接口：设置是否静音左声道。
    /// @param mute 是否静音左声道。
    void set_mute_left(bool mute)
    {
        if ( mute ) {
            set_channel_mode(MixBusChannelMode::MuteLeft);
        } else if ( get_channel_mode() == MixBusChannelMode::MuteLeft ) {
            set_channel_mode(MixBusChannelMode::Stereo);
        }
    }

    /// @brief 兼容旧接口：设置是否静音右声道。
    /// @param mute 是否静音右声道。
    void set_mute_right(bool mute)
    {
        if ( mute ) {
            set_channel_mode(MixBusChannelMode::MuteRight);
        } else if ( get_channel_mode() == MixBusChannelMode::MuteRight ) {
            set_channel_mode(MixBusChannelMode::Stereo);
        }
    }

    /// @brief 兼容旧接口：获取左声道是否处于静音模式。
    /// @return 左声道静音时返回 true。
    bool is_mute_left() const
    {
        return get_channel_mode() == MixBusChannelMode::MuteLeft;
    }

    /// @brief 兼容旧接口：获取右声道是否处于静音模式。
    /// @return 右声道静音时返回 true。
    bool is_mute_right() const
    {
        return get_channel_mode() == MixBusChannelMode::MuteRight;
    }

    /// @brief 获取左声道实时电平。
    /// @return 左声道电平。
    float get_left_level() const
    {
        return m_leftLevel.load(std::memory_order_relaxed);
    }

    /// @brief 获取右声道实时电平。
    /// @return 右声道电平。
    float get_right_level() const
    {
        return m_rightLevel.load(std::memory_order_relaxed);
    }

private:
    /// @brief 控制线程发布、音频线程只读的不可变来源快照。
    struct SourceSnapshot;

    /// @brief 发布 m_controlSources 的新快照。
    /// @warning 调用方必须持有 m_controlMutex。
    void publishSourceSnapshotLocked();

    /// @brief 回收不再被 hazard 指针保护的历史快照。
    /// @warning 调用方必须持有 m_controlMutex。
    void reclaimRetiredSourcesLocked();

    /// @brief 在 block 起点取得并保护稳定来源快照。
    /// @return 当前稳定快照。
    /// @warning 音频回调热路径：使用顺序一致原子完成单读取者 hazard 协议，
    /// 不得改为 shared_ptr 或锁。
    const SourceSnapshot* acquireSourceSnapshot() noexcept;

    /// @brief 结束当前 block 的快照读取临界区。
    /// @warning 音频回调热路径：只清除 hazard 指针，不执行内存回收。
    void releaseSourceSnapshot() noexcept;

    /// @brief 应用声道路由并更新电平。
    /// @param buffer 已完成混音的输出缓冲区。
    /// @warning 音频回调热路径：不得引入锁或内存分配。
    void finalizeOutput(AudioBuffer& buffer) noexcept;

    /// @brief 控制线程维护的稳定插入顺序来源列表。
    std::vector<std::shared_ptr<IAudioNode>> m_controlSources;

    /// @brief 当前已发布快照的独占所有权。
    std::unique_ptr<SourceSnapshot> m_activeSnapshotOwner;

    /// @brief 等待音频线程越过读取临界区的历史快照。
    std::vector<std::unique_ptr<SourceSnapshot>> m_retiredSnapshots;

    /// @brief 音频线程在 block 起点读取的当前快照地址。
    /// @warning 控制线程发布、单一音频线程读取；顺序一致语义与 hazard
    /// 指针共同阻止读取窗口内的提前回收。
    std::atomic<const SourceSnapshot*> m_activeSnapshot{ nullptr };

    /// @brief 单一音频线程当前保护的快照地址。
    /// @warning 音频线程写、控制线程读；顺序一致语义用于安全回收快照。
    std::atomic<const SourceSnapshot*> m_hazardSnapshot{ nullptr };

    /// @brief 串行化控制线程的来源修改、预备和快照回收。
    mutable std::mutex m_controlMutex;

    /// @brief 控制线程预分配、音频线程复用的临时混音缓冲区。
    AudioBuffer m_tempBuffer;

    /// @brief 临时缓冲区对应的音频格式。
    AudioDataFormat m_preparedFormat{};

    /// @brief 单次无分块处理的最大帧数。
    std::size_t m_maxPreparedFrames{ 0U };

    /// @brief 临时缓冲区是否已完成有效预备。
    bool m_isPrepared{ false };

    /// @brief 双声道输出模式。
    /// @warning 音频线程读取、控制线程写入；relaxed 足以发布独立枚举值。
    std::atomic<uint8_t> m_channelMode{ static_cast<uint8_t>(
        MixBusChannelMode::Stereo) };

    /// @brief 左声道电平。
    /// @warning 音频线程写、UI 线程读；relaxed 足以提供显示用近似值。
    std::atomic<float> m_leftLevel{ 0.0F };

    /// @brief 右声道电平。
    /// @warning 音频线程写、UI 线程读；relaxed 足以提供显示用近似值。
    std::atomic<float> m_rightLevel{ 0.0F };

    /// @brief 超出预备帧数后执行分块处理的累计次数。
    /// @warning 音频线程递增、诊断线程读取；relaxed 足以提供累计诊断值。
    std::atomic<std::uint64_t> m_oversizedProcessCount{ 0U };

    /// @brief 未预备或格式不匹配时拒绝处理的累计次数。
    /// @warning 音频线程递增、诊断线程读取；relaxed 足以提供累计诊断值。
    std::atomic<std::uint64_t> m_rejectedProcessCount{ 0U };
};
}  // namespace ice

#endif  // ICE_MIXBUS_HPP
