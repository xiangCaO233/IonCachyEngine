#include "ice/core/SourceNode.hpp"

#include "ice/config/config.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

namespace ice
{
/// @brief 发布后不再修改的参考时钟快照，避免音频线程复制回调所有权。
struct SourceNode::ReferenceProviderState {
    /// @brief 不拥有的轻量 provider 上下文。
    const void* context{ nullptr };

    /// @brief 轻量 provider 读取函数。
    ReferencePositionReader reader{ nullptr };

    /// @brief 兼容旧接口的不可变 std::function。
    std::function<std::size_t()> compatibilityProvider;

    /// @brief 查询状态是否包含有效 provider。
    /// @return 任一 provider 有效时返回 true。
    /// @warning 绝对调度的音频热路径查询；只检查入口存在，不访问外部资源。
    bool valid() const noexcept
    {
        // 这里只判断是否存在读取入口，不能验证外部 context 是否仍存活。
        return reader != nullptr || static_cast<bool>(compatibilityProvider);
    }

    /// @brief 读取当前参考帧。
    /// @return provider 未设置时返回零。
    /// @warning 音频回调热路径；兼容 provider 由调用方保证无异常和无分配。
    std::size_t read() const
    {
        // 轻量接口优先；仅在它缺失时调用兼容接口，不同时读取两个时钟。
        if ( reader ) return reader(context);
        if ( compatibilityProvider ) return compatibilityProvider();
        return 0U;
    }
};

/// @brief 接管共享音轨，并在控制阶段完成首次解码等待。
/// @warning 构造可能分配或等待后台任务，不能在音频回调内执行。
SourceNode::SourceNode(std::shared_ptr<AudioTrack> track)
    : m_track(std::move(track))
{
    // 总帧数在播放前固定，后续 EOF 检测不需要等待解码任务。
    // 音轨所有权只在节点生命周期边界变化，不在每个 block 复制。
    if ( m_track ) {
        m_totalFrames = m_track->num_frames();
    }

    // 空快照也有稳定地址，读取方无需操作控制侧的 unique_ptr。
    auto initialProvider = std::make_unique<ReferenceProviderState>();
    m_activeProvider.store(initialProvider.get(), std::memory_order_seq_cst);
    m_activeProviderOwner = std::move(initialProvider);
}

/// @brief 在音频线程停止访问后释放音轨、回调及所有 provider 快照。
/// @warning 析构不是 hazard 协议中的并发回收入口，调用方必须先停止 process。
SourceNode::~SourceNode() = default;

/// @brief 按块消费音轨，处理起播调度、结束通知、增益和显示电平。
/// @warning 每音频块调用；禁止加锁、分配、复制 shared_ptr 或等待资源。
/// 播放标志由控制侧 release 发布，此处 acquire 后读取已准备好的资源。
/// 游标、调度和增益的原子值用于跨线程控制，不提供多字段事务快照。
/// @param buffer 已预分配的内部格式输出块，有效长度就是本次请求长度。
/// 起播延迟占据输出块前缀，但不会消耗源音轨采样。
/// 结束信号描述输入耗尽，下游效果器仍可继续输出内部尾音。
void SourceNode::process(AudioBuffer& buffer)
{
    // 所有提前返回路径默认输出静音，不能泄漏上一轮复用缓冲的内容。
    buffer.clear();
    // 暂停分支只清理输出，不推进起播倒计时，也不触发结束通知。
    if ( !m_track || !m_isPlaying.load(std::memory_order_acquire) ) return;

    // 热路径拒绝不匹配格式，不临时重建缓冲或做格式转换。
    // 诊断计数不发布其他状态，只需 relaxed 累计。
    if ( buffer.afmt != ICEConfig::internal_format ) {
        m_rejectedProcessCount.fetch_add(1U, std::memory_order_relaxed);
        updateLevels(buffer, false);
        return;
    }

    // 空块不推进游标或调度，仅让显示电平衰减。
    const std::size_t requestedFrames = buffer.num_frames();
    if ( requestedFrames == 0U ) {
        updateLevels(buffer, false);
        return;
    }

    std::size_t gainedThisBlock{ 0U };
    std::size_t silenceFrames{ 0U };
    bool        startedInsideBlock{ false };

    // 相对延迟处于输出帧域，先于绝对参考时钟模式检查。
    // 延迟只通过已处理的 block 消耗，不阻塞线程等待墙钟时间。
    const std::size_t relativeDelay =
        m_scheduledStartDelayFrames.load(std::memory_order_relaxed);
    if ( relativeDelay > 0U ) {
        // 恰好等于整块时仍全部静音，下一块从第一帧开始播放。
        if ( relativeDelay >= requestedFrames ) {
            m_scheduledStartDelayFrames.store(relativeDelay - requestedFrames,
                                              std::memory_order_relaxed);
            updateLevels(buffer, false);
            return;
        }
        // 延迟落在块内时，后缀读取 PCM，前缀保留静音。
        silenceFrames = relativeDelay;
        m_scheduledStartDelayFrames.store(0U, std::memory_order_relaxed);
        startedInsideBlock = true;
    } else if ( const std::size_t scheduledStart =
                    m_scheduledStartFrame.load(std::memory_order_relaxed);
                scheduledStart > 0U ) {
        // hazard
        // 保活快照及其拥有的捕获资源，不延长裸指针或引用捕获对象的生命周期。
        // 只取一次参考帧，避免同一块的调度基准随回调读数变化。
        const ReferenceProviderState* provider = acquireReferenceProvider();
        const bool        providerValid        = provider && provider->valid();
        const std::size_t currentReference =
            providerValid ? provider->read() : 0U;
        // 参考帧已复制到局部值，后续不再访问快照即可解除保护。
        releaseReferenceProvider();

        // 没有时钟时保留绝对调度请求，不能把零值当成有效时钟起点。
        if ( !providerValid ) {
            updateLevels(buffer, false);
            return;
        }

        if ( currentReference < scheduledStart ) {
            // 先比较再相减，避免无符号帧数在目标已到达时下溢。
            const std::size_t framesToWait = scheduledStart - currentReference;
            if ( framesToWait >= requestedFrames ) {
                updateLevels(buffer, false);
                return;
            }

            // 目标落在本块内，消费一次调度请求后进入普通连续读取。
            silenceFrames = framesToWait;
            m_scheduledStartFrame.store(0U, std::memory_order_relaxed);
            startedInsideBlock = true;
        } else {
            // 已错过目标帧时立即起播，不为补偿过去时间跳读音轨。
            m_scheduledStartFrame.store(0U, std::memory_order_relaxed);
        }
    }

    // 两个读取分支都只按实际成功读取的源帧数推进音轨游标。
    // 静音等待帧属于输出时间，不属于源音轨已经消费的帧数。
    if ( startedInsideBlock ) {
        const std::size_t framesToRead = requestedFrames - silenceFrames;
        const std::size_t playbackPosition =
            m_playbackPosition.load(std::memory_order_relaxed);
        // 解码接口写到缓冲起点，随后用重叠安全移动放到块内起播位置。
        gainedThisBlock = m_track->read(buffer, playbackPosition, framesToRead);
        // 限制后续移动和游标计算的帧数；解码器仍须遵守实际写入容量。
        if ( gainedThisBlock > framesToRead ) {
            gainedThisBlock = framesToRead;
        }
        // 之前整块已清零，移动后只需补回前缀，未覆盖尾部仍为静音。
        shiftDecodedFrames(buffer, silenceFrames, gainedThisBlock);
        m_playbackPosition.store(playbackPosition + gainedThisBlock,
                                 std::memory_order_relaxed);
    } else if ( m_scheduledStartFrame.load(std::memory_order_relaxed) == 0U ) {
        // 普通连续播放直接占用整块请求容量，不进行额外帧域换算。
        const std::size_t playbackPosition =
            m_playbackPosition.load(std::memory_order_relaxed);
        gainedThisBlock =
            m_track->read(buffer, playbackPosition, requestedFrames);
        if ( gainedThisBlock > requestedFrames ) {
            gainedThisBlock = requestedFrames;
        }
        // 短读后的尾部不参与播放，明确清理可兼容解码器内部复用策略。
        if ( gainedThisBlock < requestedFrames ) {
            buffer.clear_from(gainedThisBlock);
        }
        m_playbackPosition.store(playbackPosition + gainedThisBlock,
                                 std::memory_order_relaxed);
    }

    const std::size_t playbackPosition =
        m_playbackPosition.load(std::memory_order_relaxed);
    // EOF 根据实际源帧游标判断，不以本次是否读满一整块为唯一依据。
    // 空音轨从零帧即视为结束，沿用相同的循环/单次结束通知路径。
    const bool reachedEnd = playbackPosition >= m_totalFrames;
    if ( reachedEnd ) {
        const bool looping = m_isLooping.load(std::memory_order_relaxed);
        // 循环只重置下一块游标；本块不足部分不再次从音轨头部填充。
        if ( looping ) {
            m_playbackPosition.store(0U, std::memory_order_relaxed);
        } else {
            m_playbackPosition.store(m_totalFrames, std::memory_order_relaxed);
            // 非循环结束固定在总帧数，避免之后进度通知超过音轨尾部。
            pause();
        }

        // 回调集合在处理期间保持稳定，按引用访问避免所有权增减。
        // 回调运行在音频线程，接收者不得在此改集合或执行阻塞操作。
        for ( const auto& callback : m_callbacks ) {
            callback->play_done(looping);
        }

        if ( !looping ) {
            // 最后一块仍可能含有效 PCM，通知下游同时准备处理 final 尾部。
            notifyFinalInput();
        }

        // 纯空末块不重复发送位置更新，但仍维持显示电平衰减。
        if ( gainedThisBlock == 0U ) {
            updateLevels(buffer, false);
            return;
        }
    }

    // 循环时此位置已归零，通知表示下一次读取位置，而非最后输出帧。
    const std::size_t publishedPosition =
        m_playbackPosition.load(std::memory_order_relaxed);
    for ( const auto& callback : m_callbacks ) {
        // 帧与时间通知使用同一局部游标，避免两次原子读取观察到不同位置。
        callback->frameplaypos_updated(publishedPosition);
        // 用内部采样率将源帧索引转为时间，不混入等待用的静音帧。
        using DoubleSeconds = std::chrono::duration<double>;
        callback->timeplaypos_updated(
            std::chrono::duration_cast<std::chrono::nanoseconds>(DoubleSeconds(
                static_cast<double>(publishedPosition) /
                static_cast<double>(ICEConfig::internal_format.samplerate))));
    }

    // 一个 block 使用同一增益快照，避免逐采样读取原子控制参数。
    const float gain = m_volume.load(std::memory_order_relaxed);
    if ( std::abs(gain - 1.0F) > std::numeric_limits<float>::epsilon() ) {
        applyVolume(buffer, gain);
    }
    // 峰值统计的是最终增益后的输出；空读仅触发已有峰值衰减。
    updateLevels(buffer, gainedThisBlock > 0U);
}

/// @brief 控制线程将兼容回调移动进新快照后发布。
/// @warning 可分配并回收捕获对象，不能从音频线程调用。
void SourceNode::set_reference_pos_provider(
    std::function<std::size_t()> provider)
{
    // 捕获资源归新快照所有，旧快照中的捕获仍由退役协议管理。
    auto state                   = std::make_unique<ReferenceProviderState>();
    state->compatibilityProvider = std::move(provider);
    publishReferenceProvider(std::move(state));
}

/// @brief 发布函数指针及非拥有上下文，不延长上下文对象生命周期。
/// @param context 调用方维持有效的参考时钟对象。
/// @param reader 读取当前参考帧的无异常函数，空值表示取消读取入口。
/// @warning 控制路径仍会分配快照；context 必须覆盖所有并发读操作。
void SourceNode::set_reference_pos_provider(const void*             context,
                                            ReferencePositionReader reader)
{
    auto state = std::make_unique<ReferenceProviderState>();
    // 空 reader 同时清空上下文，避免无效状态仍携带看似可用的地址。
    state->context = reader ? context : nullptr;
    state->reader  = reader;
    publishReferenceProvider(std::move(state));
}

/// @brief 发布空时钟快照，旧快照在无读取者保护后回收。
/// @warning 控制线程操作，清除不等于立即销毁仍在执行的回调。
void SourceNode::clear_reference_pos_provider()
{
    // 使用空状态而非直接销毁活动对象，允许正在读取的旧快照自然退出。
    publishReferenceProvider(std::make_unique<ReferenceProviderState>());
}

/// @brief 控制侧显式清理退役快照，供停止播放等低频阶段使用。
/// @warning 获取控制锁并可能析构回调捕获对象，禁止在音频回调执行。
void SourceNode::reclaimRetiredReferenceProviders()
{
    std::lock_guard<std::mutex> lock(m_providerControlMutex);
    // 与并发发布共用同一把控制锁，避免同时修改退役容器。
    reclaimRetiredReferenceProvidersLocked();
}

/// @brief 在控制锁保护下取得退役队列大小，仅用于诊断。
/// @warning 查询会加锁，不应逐音频块调用。
std::size_t SourceNode::retiredReferenceProviderCount() const
{
    std::lock_guard<std::mutex> lock(m_providerControlMutex);
    // 返回值仅是此刻快照数量，不能用作外部上下文可以销毁的证明。
    return m_retiredProviders.size();
}

/// @brief 先建立快照所有权，再发布地址并回收旧状态。
/// @warning 控制线程可加锁与扩容；seq_cst 发布与音频侧 hazard 共同保证存活。
void SourceNode::publishReferenceProvider(
    std::unique_ptr<ReferenceProviderState> state)
{
    // 空参数归一为有效的空快照，读侧可以沿用同一套获取协议。
    if ( !state ) {
        state = std::make_unique<ReferenceProviderState>();
    }

    std::lock_guard<std::mutex>   lock(m_providerControlMutex);
    const ReferenceProviderState* nextAddress = state.get();
    // 旧对象先进入退役集合，不能在替换 unique_ptr 时直接析构。
    if ( m_activeProviderOwner ) {
        m_retiredProviders.push_back(std::move(m_activeProviderOwner));
    }
    // 地址对应对象已完整构造，随后发布后禁止再原地修改其回调字段。
    m_activeProviderOwner = std::move(state);
    // 读者只观察发布地址，不接触此锁内变更的所有权容器。
    m_activeProvider.store(nextAddress, std::memory_order_seq_cst);
    reclaimRetiredReferenceProvidersLocked();
}

/// @brief 保留当前 hazard 指向的快照，销毁其余退役对象。
/// @warning 必须持有控制锁；该协议仅支持一个不可重入的音频读取者。
void SourceNode::reclaimRetiredReferenceProvidersLocked()
{
    // 仅保护槽中当前地址；单读取者约束不能通过增大退役队列替代。
    const ReferenceProviderState* protectedProvider =
        m_providerHazard.load(std::memory_order_seq_cst);
    // hazard 只保护回调快照，不自动保护轻量接口的外部 context。
    // 与发布地址同属 seq_cst 顺序，避免读取者已经确认的对象被提前释放。
    std::erase_if(m_retiredProviders,
                  [protectedProvider](
                      const std::unique_ptr<ReferenceProviderState>& provider) {
                      // 未被保护的对象在控制侧析构，释放捕获资源不落入音频回调。
                      return provider.get() != protectedProvider;
                  });
}

/// @brief 发布读取意图并复查活动地址，取得可安全解引用的快照。
/// @warning 音频热路径；只访问 seq_cst 原子，不得改为互斥锁或共享所有权。
/// 控制侧持续发布时可反复重试，不保证固定次数或固定回调耗时。
const SourceNode::ReferenceProviderState*
SourceNode::acquireReferenceProvider() noexcept
{
    const ReferenceProviderState* provider{ nullptr };
    // 首次读地址后暂不解引用：控制侧可能正在退役并销毁该旧对象。
    do {
        provider = m_activeProvider.load(std::memory_order_seq_cst);
        // 只有发布 hazard 后二次读仍一致，才允许访问快照内容。
        m_providerHazard.store(provider, std::memory_order_seq_cst);
    } while ( provider != m_activeProvider.load(std::memory_order_seq_cst) );
    // 后续使用必须与 release 成对，且不能嵌套覆盖同一个 hazard 槽。
    return provider;
}

/// @brief 解除当前音频读取保护，将实际对象回收留在控制侧。
/// @warning 每次获取后调用；seq_cst 清空参与跨线程回收顺序。
void SourceNode::releaseReferenceProvider() noexcept
{
    // 清空后禁止再解引用之前的 provider，控制侧可能立即回收它。
    m_providerHazard.store(nullptr, std::memory_order_seq_cst);
}

/// @brief 对一次播放周期只发送一次输入结束信号。
/// @warning 音频热路径；控制侧复位、音频侧置位，监听器必须稳定且不阻塞。
void SourceNode::notifyFinalInput() noexcept
{
    // exchange 防止多个结束分支重复通知；重新定位会为新周期复位标志。
    if ( m_finalInputNotified.exchange(true, std::memory_order_acq_rel) ) {
        return;
    }
    // 函数和上下文仅能在处理停止时更换，此处无需复制可变回调对象。
    if ( m_finalInputListener ) {
        m_finalInputListener(m_finalInputListenerContext);
    }
}

/// @brief 原地后移有效 PCM，构造块内延迟起播所需的静音前缀。
/// @param buffer 已在 process 起点清零、随后从首帧写入 PCM 的输出块。
/// @param silenceFrames 本块需要保留的前缀静音长度。
/// @param decodedFrames 已写入缓冲起点的每声道有效帧数。
/// @warning 音频热路径；只操作已有容量，不允许分配临时缓冲。
void SourceNode::shiftDecodedFrames(AudioBuffer& buffer,
                                    std::size_t  silenceFrames,
                                    std::size_t  decodedFrames) noexcept
{
    // raw_ptrs 只借用平面地址表，不改变缓冲所有权。
    float** samples = buffer.raw_ptrs();
    if ( !samples || silenceFrames >= buffer.num_frames() ) return;

    // 静音前缀占用输出空间，有效 PCM 必须截断到剩余容量内。
    const std::size_t safeDecodedFrames =
        std::min(decodedFrames, buffer.num_frames() - silenceFrames);
    for ( std::uint16_t channel = 0U; channel < buffer.num_channels();
          ++channel ) {
        if ( safeDecodedFrames > 0U ) {
            // 源和目标处于同一声道数组，必须用 memmove 处理区间重叠。
            std::memmove(samples[channel] + silenceFrames,
                         samples[channel],
                         safeDecodedFrames * sizeof(float));
        }
        // 移动完成后再清前缀，否则会抹掉尚未搬走的有效采样。
        std::memset(samples[channel], 0, silenceFrames * sizeof(float));
    }
}

/// @brief 将本块固定线性增益应用到全部声道。
/// @param buffer 原地修改的平面 PCM，包含起播或短读形成的静音。
/// @param gain 控制参数的一次读取结果，整个块内不再重新采样该参数。
/// @warning 每块热路径，按既有容量遍历，不分配且不读取控制锁。
void SourceNode::applyVolume(AudioBuffer& buffer, float gain) noexcept
{
    float** samples = buffer.raw_ptrs();
    if ( !samples ) return;
    // 所有声道采用同一增益，不在源节点做限幅或非线性压缩。
    for ( std::uint16_t channel = 0U; channel < buffer.num_channels();
          ++channel ) {
        for ( std::size_t frame = 0U; frame < buffer.num_frames(); ++frame ) {
            samples[channel][frame] *= gain;
        }
    }
}

/// @brief 发布左右声道峰值，静音块按块衰减旧显示值。
/// @param buffer 已施加音量的最终输出块。
/// @param audible 是否有有效输入；为假时不扫描 PCM 内容。
/// @warning 音频线程写、显示线程读；relaxed 仅提供近似电平，不发布 PCM。
void SourceNode::updateLevels(const AudioBuffer& buffer, bool audible) noexcept
{
    float               maxLeft{ 0.0F };
    float               maxRight{ 0.0F };
    const float* const* samples = buffer.raw_ptrs();
    // 多声道只展示前两个通道，不能把此显示结果当作全通道限幅依据。
    if ( audible && samples ) {
        if ( buffer.num_channels() > 0U ) {
            for ( std::size_t frame = 0U; frame < buffer.num_frames();
                  ++frame ) {
                maxLeft = std::max(maxLeft, std::abs(samples[0][frame]));
            }
        }
        if ( buffer.num_channels() > 1U ) {
            for ( std::size_t frame = 0U; frame < buffer.num_frames();
                  ++frame ) {
                maxRight = std::max(maxRight, std::abs(samples[1][frame]));
            }
        } else if ( buffer.num_channels() > 0U ) {
            // 单声道在左右表头显示同一幅值，不更改实际 PCM 声道布局。
            maxRight = maxLeft;
        }
    }

    // 下降乘数按处理块次数生效，不是按秒标定的响度或包络指标。
    // 左右值独立发布，显示侧不应要求两次读取组成严格同步快照。
    const float previousLeft  = m_leftLevel.load(std::memory_order_relaxed);
    const float previousRight = m_rightLevel.load(std::memory_order_relaxed);
    m_leftLevel.store(maxLeft > previousLeft ? maxLeft : previousLeft * 0.95F,
                      std::memory_order_relaxed);
    m_rightLevel.store(
        maxRight > previousRight ? maxRight : previousRight * 0.95F,
        std::memory_order_relaxed);
}
}  // namespace ice
