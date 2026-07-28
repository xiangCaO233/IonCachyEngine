#ifndef ICE_SOURCENODE_HPP
#define ICE_SOURCENODE_HPP

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <vector>

#include "ice/core/IAudioNode.hpp"
#include "ice/core/PlayCallBack.hpp"
#include "ice/manage/AudioTrack.hpp"

namespace ice
{
/// @brief 从音轨按独立播放位置输出 PCM 的音频源节点。
class SourceNode : public IAudioNode
{
public:
    /// @brief 不持有上下文的轻量参考时钟读取函数。
    ///
    /// AudioManager 可将生命周期覆盖音频回调的时间线节点作为 context，
    /// 由静态函数读取其中的原子 block 时钟，避免在回调内访问可变
    /// std::function。
    using ReferencePositionReader =
        std::size_t (*)(const void* context) noexcept;

    /// @brief 不持有上下文的轻量输入结束通知函数。
    using FinalInputListener = void (*)(void* context) noexcept;

    /// @brief 构造音源节点。
    /// @param track 节点生命周期内保持有效的音轨。
    /// @warning 低频控制路径：会等待 Cachy 音轨完成首次解码，避免音频回调
    /// 在 IDecoder::decode 中阻塞或接管异步结果。
    explicit SourceNode(std::shared_ptr<AudioTrack> track);

    /// @brief 析构音源节点。
    /// @warning 销毁前必须停止调用 process 的音频线程。
    ~SourceNode() override;

    SourceNode(const SourceNode&)            = delete;
    SourceNode& operator=(const SourceNode&) = delete;
    SourceNode(SourceNode&&)                 = delete;
    SourceNode& operator=(SourceNode&&)      = delete;

    /// @brief 从当前播放位置读取请求帧并填充缓冲区。
    /// @param buffer 调用方预分配且格式为 ICEConfig::internal_format 的缓冲区。
    /// @warning 音频回调热路径：不得引入锁、内存分配或 provider 状态复制；
    /// 同一 SourceNode 只允许一个音频线程调用且不可重入。
    void process(AudioBuffer& buffer) override;

    /// @brief 查询音源是否正在播放。
    /// @return 正在播放时返回 true。
    bool isplaying() const
    {
        return m_isPlaying.load(std::memory_order_acquire);
    }

    /// @brief 暂停音源。
    void pause() { m_isPlaying.store(false, std::memory_order_release); }

    /// @brief 开始或继续播放音源。
    void play() { m_isPlaying.store(true, std::memory_order_release); }

    /// @brief 获取线性音量。
    /// @return 当前线性音量。
    float getvolume() const { return m_volume.load(std::memory_order_relaxed); }

    /// @brief 设置线性音量。
    /// @param value 新的线性音量。
    void setvolume(float value)
    {
        m_volume.store(value, std::memory_order_relaxed);
    }

    /// @brief 设置是否循环播放。
    /// @param enabled 是否在结束后回到零帧。
    void setloop(bool enabled)
    {
        m_isLooping.store(enabled, std::memory_order_relaxed);
    }

    /// @brief 查询是否循环播放。
    /// @return 循环启用时返回 true。
    bool isloop() const { return m_isLooping.load(std::memory_order_relaxed); }

    /// @brief 获取音轨内播放帧位置。
    /// @return 下一次读取的源帧。
    std::size_t get_playpos() const
    {
        return m_playbackPosition.load(std::memory_order_relaxed);
    }

    /// @brief 设置音轨内播放帧位置。
    /// @param framePosition 新的源帧。
    void set_playpos(std::size_t framePosition)
    {
        m_playbackPosition.store(framePosition, std::memory_order_relaxed);
        m_finalInputNotified.store(false, std::memory_order_relaxed);
    }

    /// @brief 添加播放进度回调。
    /// @param callback 回调对象。
    /// @warning 只能在 process 停止时修改回调集合。
    void add_playcallback(const std::shared_ptr<PlayCallBack>& callback)
    {
        m_callbacks.insert(callback);
    }

    /// @brief 移除播放进度回调。
    /// @param callback 回调对象。
    /// @warning 只能在 process 停止时修改回调集合。
    void remove_playcallback(const std::shared_ptr<PlayCallBack>& callback)
    {
        m_callbacks.erase(callback);
    }

    /// @brief 获取左声道实时电平。
    /// @return 左声道峰值。
    float get_left_level() const
    {
        return m_leftLevel.load(std::memory_order_relaxed);
    }

    /// @brief 获取右声道实时电平。
    /// @return 右声道峰值。
    float get_right_level() const
    {
        return m_rightLevel.load(std::memory_order_relaxed);
    }

    /// @brief 按时间设置音轨内播放位置。
    /// @tparam Rep duration 数值类型。
    /// @tparam Period duration 时间比例。
    /// @param timePosition 从音轨起始点计算的时间。
    template<typename Rep, typename Period>
    void set_playpos(const std::chrono::duration<Rep, Period>& timePosition)
    {
        const auto sampleRate =
            static_cast<double>(ICEConfig::internal_format.samplerate);
        if ( sampleRate <= 0.0 || !m_track ) return;

        using DoubleSeconds = std::chrono::duration<double>;
        const auto seconds =
            std::chrono::duration_cast<DoubleSeconds>(timePosition).count();
        const auto requestedFrame =
            static_cast<std::size_t>(seconds * sampleRate);
        set_playpos(std::min(requestedFrame, m_totalFrames));
    }

    /// @brief 设置相对于参考时钟的预定开始帧。
    /// @param frame 参考时间线上的目标帧；零表示取消预定。
    void set_scheduled_start_frame(std::size_t frame)
    {
        m_scheduledStartDelayFrames.store(0U, std::memory_order_relaxed);
        m_scheduledStartFrame.store(frame, std::memory_order_relaxed);
    }

    /// @brief 获取当前尚未触发的预定开始帧。
    /// @return 目标参考帧；零表示没有预定。
    std::size_t scheduledStartFrame() const
    {
        return m_scheduledStartFrame.load(std::memory_order_relaxed);
    }

    /// @brief 设置相对于后续 process 输出帧域的起播延迟。
    /// @param frames 起播前需要跳过的输出帧数；零表示立即播放。
    ///
    /// 此模式不依赖外部参考时钟，适用于节点位于全局变速器之后的路由。
    /// 设置后会清除绝对参考帧调度。
    void set_scheduled_start_delay_frames(std::size_t frames)
    {
        m_scheduledStartFrame.store(0U, std::memory_order_relaxed);
        m_scheduledStartDelayFrames.store(frames, std::memory_order_relaxed);
    }

    /// @brief 获取相对输出帧域内尚需等待的帧数。
    /// @return 零表示没有相对延迟。
    std::size_t scheduledStartDelayFrames() const
    {
        return m_scheduledStartDelayFrames.load(std::memory_order_relaxed);
    }

    /// @brief 兼容旧接口：发布不可变 std::function 参考时钟。
    /// @param provider 返回当前参考帧的回调；空回调表示清除。
    /// @warning
    /// 控制线程接口，会分配并回收快照；provider 必须无异常、无阻塞、无分配，
    /// 且其捕获对象生命周期必须覆盖调用。实时路径优先使用轻量重载。
    void set_reference_pos_provider(std::function<std::size_t()> provider);

    /// @brief 发布不拥有上下文的轻量参考时钟。
    /// @param context 生命周期覆盖全部 process 调用的常驻上下文。
    /// @param reader 无异常、无阻塞、无分配的读取函数；空值表示清除。
    /// @warning 控制线程接口，会分配并回收不可变 provider 快照。
    void set_reference_pos_provider(const void*             context,
                                    ReferencePositionReader reader);

    /// @brief 清除参考时钟。
    /// @warning 控制线程接口，会发布新的空快照。
    void clear_reference_pos_provider();

    /// @brief 设置在最后一批有效输入所在 process 内触发的结束通知。
    /// @param context 生命周期覆盖当前节点的非拥有上下文。
    /// @param listener 无异常、无阻塞、无分配的通知函数。
    /// @warning 只能在 process 停止时设置；监听器及上下文必须保持稳定。
    void set_final_input_listener(void*              context,
                                  FinalInputListener listener) noexcept
    {
        m_finalInputListenerContext = listener ? context : nullptr;
        m_finalInputListener        = listener;
    }

    /// @brief 清除输入结束通知。
    /// @warning 只能在 process 停止时调用。
    void clear_final_input_listener() noexcept
    {
        m_finalInputListenerContext = nullptr;
        m_finalInputListener        = nullptr;
    }

    /// @brief 回收不再由音频线程保护的旧 provider 快照。
    /// @warning 控制线程接口，可能释放内存并获取锁，不得在 process 调用。
    void reclaimRetiredReferenceProviders();

    /// @brief 获取等待回收的 provider 快照数量。
    /// @return 当前退役快照数量。
    /// @warning 诊断接口，会获取控制锁，不得在 process 调用。
    std::size_t retiredReferenceProviderCount() const;

    /// @brief 获取音轨总帧数。
    /// @return 音轨为空时返回零。
    std::size_t num_frames() const { return m_totalFrames; }

    /// @brief 获取音轨原始格式。
    /// @return 音轨媒体格式。
    const AudioDataFormat& format() const
    {
        return m_track->get_media_info().format;
    }

    /// @brief 获取音轨总时长。
    /// @return 采样率或音轨无效时返回零纳秒。
    [[nodiscard]] std::chrono::nanoseconds total_time() const
    {
        const std::size_t totalFrames = num_frames();
        const auto        sampleRate =
            static_cast<double>(ICEConfig::internal_format.samplerate);
        if ( sampleRate <= 0.0 || totalFrames == 0U ) {
            return std::chrono::nanoseconds(0);
        }

        using DoubleSeconds = std::chrono::duration<double>;
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            DoubleSeconds(static_cast<double>(totalFrames) / sampleRate));
    }

    /// @brief 获取格式不符合内部混音格式而输出静音的 block 次数。
    /// @return 被拒绝的 block 累计数量。
    std::uint64_t rejectedProcessCount() const
    {
        return m_rejectedProcessCount.load(std::memory_order_relaxed);
    }

private:
    /// @brief 控制线程发布、音频线程只读的不可变参考时钟状态。
    struct ReferenceProviderState;

    /// @brief 发布新 provider 状态。
    /// @param state 已在控制线程完整构造的状态。
    void publishReferenceProvider(
        std::unique_ptr<ReferenceProviderState> state);

    /// @brief 回收未被 hazard 指针保护的 provider 状态。
    /// @warning 调用方必须持有 m_providerControlMutex。
    void reclaimRetiredReferenceProvidersLocked();

    /// @brief 在 block 起点取得稳定 provider 状态。
    /// @return 当前 provider 状态。
    /// @warning 音频回调热路径：单读取者 hazard 协议，不得改为锁或 shared_ptr。
    const ReferenceProviderState* acquireReferenceProvider() noexcept;

    /// @brief 结束当前 provider 读取临界区。
    /// @warning 音频回调热路径：只清除 hazard，不执行对象回收。
    void releaseReferenceProvider() noexcept;

    /// @brief 对当前播放周期发送一次输入结束通知。
    /// @warning 音频回调热路径：仅访问 lock-free 原子并调用稳定函数指针。
    void notifyFinalInput() noexcept;

    /// @brief 将已读取到缓冲区起点的 PCM 原地移动到 block 内起播位置。
    /// @param buffer 输出缓冲区。
    /// @param silenceFrames block 起点需要保留的静音帧数。
    /// @param decodedFrames 已读取的有效 PCM 帧数。
    /// @warning 音频回调热路径：仅使用 memmove 和 memset，不分配内存。
    static void shiftDecodedFrames(AudioBuffer& buffer,
                                   std::size_t  silenceFrames,
                                   std::size_t  decodedFrames) noexcept;

    /// @brief 应用线性音量。
    /// @param buffer 输出缓冲区。
    /// @param gain 本 block 固定的线性增益。
    /// @warning 音频回调热路径：不得引入分配或锁。
    static void applyVolume(AudioBuffer& buffer, float gain) noexcept;

    /// @brief 更新 block 峰值。
    /// @param buffer 已完成音量处理的输出缓冲区。
    /// @param audible 本 block 是否应统计音频。
    /// @warning 音频回调热路径：只遍历输出缓冲并访问 relaxed 原子。
    void updateLevels(const AudioBuffer& buffer, bool audible) noexcept;

    /// @brief 保持解码资源存活的音轨。
    std::shared_ptr<AudioTrack> m_track;

    /// @brief 构造阶段完成解码等待后固定的音轨总帧数。
    ///
    /// 音频回调直接读取该不可变值，避免再次进入解码器的 call_once 快路径。
    std::size_t m_totalFrames{ 0U };

    /// @brief 只允许在 process 停止时修改的播放回调列表。
    std::set<std::shared_ptr<PlayCallBack>> m_callbacks;

    /// @brief 下一次读取的音轨源帧。
    /// @warning 控制线程和音频线程均可写；relaxed 提供无锁定位语义。
    std::atomic<std::size_t> m_playbackPosition{ 0U };

    /// @brief 线性音量。
    /// @warning 控制线程写、音频线程逐 block 读取；relaxed 足够。
    std::atomic<float> m_volume{ 0.4F };

    /// @brief 播放结束后是否回到零帧。
    /// @warning 控制线程写、音频线程读取；relaxed 足够。
    std::atomic_bool m_isLooping{ false };

    /// @brief 音源是否正在播放。
    /// @warning 控制线程发布配置、音频线程读取；release/acquire 用于可见性。
    std::atomic_bool m_isPlaying{ false };

    /// @brief 相对于参考时钟的预定目标帧。
    /// @warning 控制线程写、音频线程读取并清零；relaxed 与 play 的发布配合。
    std::atomic<std::size_t> m_scheduledStartFrame{ 0U };

    /// @brief 相对于后续 process 输出帧域的剩余起播延迟。
    /// @warning 控制线程写、音频线程读取并递减；relaxed 与 play 发布配合。
    std::atomic<std::size_t> m_scheduledStartDelayFrames{ 0U };

    /// @brief 当前播放周期是否已经发送输入结束通知。
    /// @warning 控制线程复位、音频线程置位；避免重复通知下游 final。
    std::atomic_bool m_finalInputNotified{ false };

    /// @brief 生命周期覆盖 process 的非拥有结束通知上下文。
    void* m_finalInputListenerContext{ nullptr };

    /// @brief 最后一批输入所在 process 内调用的稳定结束通知。
    FinalInputListener m_finalInputListener{ nullptr };

    /// @brief 当前 provider 状态的控制线程所有权。
    std::unique_ptr<ReferenceProviderState> m_activeProviderOwner;

    /// @brief 等待音频线程越过读取临界区的 provider 状态。
    std::vector<std::unique_ptr<ReferenceProviderState>> m_retiredProviders;

    /// @brief 音频线程在 block 起点读取的当前 provider 地址。
    /// @warning 控制线程发布、单一音频线程读取；顺序一致语义参与 hazard 协议。
    std::atomic<const ReferenceProviderState*> m_activeProvider{ nullptr };

    /// @brief 音频线程当前保护的 provider 地址。
    /// @warning 音频线程写、控制线程读；顺序一致语义阻止提前回收。
    std::atomic<const ReferenceProviderState*> m_providerHazard{ nullptr };

    /// @brief 串行化控制线程的 provider 发布与回收。
    mutable std::mutex m_providerControlMutex;

    /// @brief 左声道峰值。
    /// @warning 音频线程写、UI 线程读；relaxed 足够显示近似值。
    std::atomic<float> m_leftLevel{ 0.0F };

    /// @brief 右声道峰值。
    /// @warning 音频线程写、UI 线程读；relaxed 足够显示近似值。
    std::atomic<float> m_rightLevel{ 0.0F };

    /// @brief 格式不匹配而输出静音的累计 block 数。
    /// @warning 音频线程递增、诊断线程读取；relaxed 足够累计统计。
    std::atomic<std::uint64_t> m_rejectedProcessCount{ 0U };
};
}  // namespace ice

#endif  // ICE_SOURCENODE_HPP
