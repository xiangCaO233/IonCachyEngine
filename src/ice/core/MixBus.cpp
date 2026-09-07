#include "ice/core/MixBus.hpp"

#include "ice/config/config.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

namespace ice
{
/// @brief 发布后不再修改的来源集合，节点所有权由控制侧创建和回收。
struct MixBus::SourceSnapshot {
    /// @brief 按控制线程插入顺序固定的来源列表。
    std::vector<std::shared_ptr<IAudioNode>> sources;
};

/// @brief 在音频线程启动前建立非空快照及默认处理容量。
/// @warning 构造期间允许分配；对象完成构造后才能发布给音频回调。
MixBus::MixBus()
{
    // 空总线也持有有效快照，使回调无需为空集合另设空指针路径。
    // 原子指针只负责发布地址，独占所有者负责快照本体的生存期。
    auto initialSnapshot = std::make_unique<SourceSnapshot>();
    m_activeSnapshot.store(initialSnapshot.get(), std::memory_order_seq_cst);
    m_activeSnapshotOwner = std::move(initialSnapshot);

    // 默认缓冲长度至少为一帧，避免默认配置为零时分块循环无法前进。
    prepare(ICEConfig::internal_format,
            std::max<std::size_t>(ICEConfig::default_buffer_size, 1U));
}

/// @brief 回收控制侧拥有的当前及历史快照。
/// @warning 必须先停止音频回调；默认析构不会等待仍持有 hazard 的读取者。
MixBus::~MixBus() = default;

/// @brief 为后续回调预备固定容量，格式无效时将总线置为拒绝处理状态。
/// @param format 所有来源必须遵守的平面音频格式。
/// @param maxFrames 临时缓冲的容量，不是最终输出块的硬上限。
/// @warning 低频控制路径会加锁和分配；调用方必须保证没有并发 process。
void MixBus::prepare(const AudioDataFormat& format, std::size_t maxFrames)
{
    // 此锁只串行化控制操作，不能保护未取锁的音频线程。
    // 因而格式及容量这组普通成员必须在回调停止时一起更新。
    std::lock_guard<std::mutex> lock(m_controlMutex);
    m_preparedFormat    = format;
    m_maxPreparedFrames = maxFrames;
    m_isPrepared =
        format.channels > 0U && format.samplerate > 0U && maxFrames > 0U;

    if ( m_isPrepared ) {
        // 分配发生在这里；process 只能调整活动帧数，不能扩大容量。
        m_tempBuffer.resize(format, maxFrames);
        m_tempBuffer.clear();
    } else {
        // 清除活动帧数，避免无效格式继续暴露上一次处理的数据。
        m_tempBuffer.resize(format, 0U);
    }
}

/// @brief 用同一来源快照完成整个输出块，必要时分块复用临时缓冲。
/// @param buffer 由调用者提供的输出空间，原有样本会被清空。
/// @warning 每个音频周期执行；仅允许单读取者，不得加锁、分配或复制共享所有权。
/// 诊断计数使用 relaxed，不承担快照发布或控制状态同步职责。
void MixBus::process(AudioBuffer& buffer)
{
    // 累加以静音为起点；空来源集合自然得到静音，不保留旧缓冲内容。
    buffer.clear();

    const std::size_t frameCount = buffer.num_frames();
    if ( frameCount == 0U ) {
        // 即使没有样本也推进显示电平衰减，不让 UI 停留在旧峰值。
        finalizeOutput(buffer);
        return;
    }

    if ( !m_isPrepared || buffer.afmt != m_preparedFormat ||
         m_maxPreparedFrames == 0U ) {
        // 回调不能临时修复格式或扩容；拒绝本块并维持确定的静音输出。
        m_rejectedProcessCount.fetch_add(1U, std::memory_order_relaxed);
        finalizeOutput(buffer);
        return;
    }

    if ( frameCount > m_maxPreparedFrames ) {
        // 超容量并非失败：后续拆成小块。此计数用于诊断调用方的块大小。
        m_oversizedProcessCount.fetch_add(1U, std::memory_order_relaxed);
    }

    // 整个外层循环只获取一次快照，避免一个输出块混用两套来源。
    // 只借用其中的共享指针引用，不在实时线程增加或减少节点引用计数。
    const SourceSnapshot* snapshot = acquireSourceSnapshot();
    std::size_t           outputOffset{ 0U };
    while ( outputOffset < frameCount ) {
        // 最后一块可短于预备容量；节点必须按活动帧数写入而非缓冲容量。
        const std::size_t chunkFrames =
            std::min(m_maxPreparedFrames, frameCount - outputOffset);

        for ( const auto& source : snapshot->sources ) {
            if ( !m_tempBuffer.set_active_frames(chunkFrames) ) {
                // 容量约束失效时放弃整个输出，而不是输出已混好的半块。
                // 所有获取快照后的提前返回都必须先解除 hazard 保护。
                releaseSourceSnapshot();
                buffer.clear();
                m_rejectedProcessCount.fetch_add(1U, std::memory_order_relaxed);
                finalizeOutput(buffer);
                return;
            }
            // 来源共享同一块临时空间，必须先清零，不能串入前一来源的数据。
            m_tempBuffer.clear();
            // 来源可能持有自己的播放游标；每次调用只消费当前 chunk 的时间跨度。
            // 因此一旦来源已处理，后续失败也不能在此重新调用它来重试整块。
            source->process(m_tempBuffer);

            if ( m_tempBuffer.afmt != m_preparedFormat ||
                 m_tempBuffer.num_frames() != chunkFrames ) {
                // 节点不得擅自改变约定格式或帧数，否则下面的逐通道累加不安全。
                // 即使此前有来源成功，也统一清空整块以保持失败语义一致。
                // 已推进的来源状态不在总线中回滚；诊断计数用于暴露这类契约违例。
                releaseSourceSnapshot();
                buffer.clear();
                m_rejectedProcessCount.fetch_add(1U, std::memory_order_relaxed);
                finalizeOutput(buffer);
                return;
            }

            float**             output = buffer.raw_ptrs();
            const float* const* input  = m_tempBuffer.raw_ptrs();
            if ( !output || !input ) {
                // 活动帧数正常仍需有平面指针；不在回调中尝试重新申请内存。
                releaseSourceSnapshot();
                buffer.clear();
                m_rejectedProcessCount.fetch_add(1U, std::memory_order_relaxed);
                finalizeOutput(buffer);
                return;
            }

            // AudioBuffer 提供平面浮点采样；输入从零开始，输出写到当前块偏移。
            // 这里仅相加，不做归一化或限幅，避免改变总线已有的增益语义。
            for ( std::uint16_t channel = 0U;
                  channel < m_preparedFormat.channels;
                  ++channel ) {
                float*       destination = output[channel] + outputOffset;
                const float* sourceData  = input[channel];
                for ( std::size_t frame = 0U; frame < chunkFrames; ++frame ) {
                    destination[frame] += sourceData[frame];
                }
            }
        }

        // 所有来源完成当前块后才前进，保持各来源在同一输出时间段叠加。
        outputOffset += chunkFrames;
    }
    // 后处理只访问输出缓冲，不再访问来源；尽早结束读取临界区。
    releaseSourceSnapshot();

    finalizeOutput(buffer);
}

/// @brief 在控制列表末尾加入唯一来源，变更通过新快照对回调可见。
/// @param src 非空输入节点，快照持有共享所有权以跨越在途输出块。
/// @warning 低频控制路径会加锁、复制共享所有权并分配快照，不得用于音频回调。
void MixBus::add_source(std::shared_ptr<IAudioNode> src)
{
    if ( !src ) return;

    std::lock_guard<std::mutex> lock(m_controlMutex);
    const auto                  existing =
        std::find(m_controlSources.begin(), m_controlSources.end(), src);
    // 重复加入同一节点会导致重复混音，因此将其视为幂等操作。
    if ( existing != m_controlSources.end() ) return;

    m_controlSources.push_back(std::move(src));
    publishSourceSnapshotLocked();
}

/// @brief 从下一次取得的来源快照中移除节点，不打断已经开始的输出块。
/// @param src 按共享指针所指对象查找；空值或不存在时不发布新版本。
/// @warning 控制线程可能加锁和回收；不能据此认为旧回调已停止访问该节点。
void MixBus::remove_source(const std::shared_ptr<IAudioNode>& src)
{
    if ( !src ) return;

    std::lock_guard<std::mutex> lock(m_controlMutex);
    const auto                  existing =
        std::find(m_controlSources.begin(), m_controlSources.end(), src);
    if ( existing == m_controlSources.end() ) return;

    // 当前音频快照仍保有节点所有权，控制列表删除不等于立即销毁节点。
    m_controlSources.erase(existing);
    publishSourceSnapshotLocked();
}

/// @brief 保留插入位置替换来源，拒绝把已经存在的节点重复放入列表。
/// @param current 必须存在于当前控制列表中的原节点。
/// @param replacement 替换后的非空节点；不能是列表中另一个位置的节点。
/// @return 找到原节点且替换有效时为 true；同节点替换也视为成功。
/// @warning 控制侧低频调用；发布前后的快照可短暂同时持有不同节点。
bool MixBus::replace_source(const std::shared_ptr<IAudioNode>& current,
                            std::shared_ptr<IAudioNode>        replacement)
{
    if ( !current || !replacement ) return false;

    std::lock_guard<std::mutex> lock(m_controlMutex);
    const auto                  currentPosition =
        std::find(m_controlSources.begin(), m_controlSources.end(), current);
    if ( currentPosition == m_controlSources.end() ) return false;
    if ( current == replacement ) return true;

    // 不允许以列表内另一个来源替代当前来源，否则它会被处理两次。
    const auto replacementPosition = std::find(
        m_controlSources.begin(), m_controlSources.end(), replacement);
    if ( replacementPosition != m_controlSources.end() ) return false;

    // 原位更新维持确定的浮点累加顺序，不使用先删除再追加的方式。
    *currentPosition = std::move(replacement);
    publishSourceSnapshotLocked();
    return true;
}

/// @brief 发布空来源集合，同时利用本次控制操作回收不受保护的旧快照。
/// @warning 不等待在途回调；空列表仅对随后取得新快照的输出块生效。
void MixBus::clear()
{
    std::lock_guard<std::mutex> lock(m_controlMutex);
    if ( m_controlSources.empty() ) {
        // 列表已经为空时无需分配新快照，但仍可能有已退出回调留下的历史版本。
        reclaimRetiredSourcesLocked();
        return;
    }

    // 发布空集合并不取消正在运行的来源 process；它们仍可完成当前块。
    // 若调用方需要立即停止所有来源访问，必须另外停止音频回调。
    m_controlSources.clear();
    publishSourceSnapshotLocked();
}

/// @brief 在控制线程主动回收已越过读取临界区的历史版本。
/// @warning 可能析构节点和释放内存；调用频率由控制侧决定，不在音频线程执行。
void MixBus::reclaimRetiredSources()
{
    // 音频线程退出读取只清空 hazard，不直接清理退休队列。
    // 即使之后没有来源变更，控制侧仍可用此入口主动完成延迟释放。
    std::lock_guard<std::mutex> lock(m_controlMutex);
    reclaimRetiredSourcesLocked();
}

/// @brief 返回控制侧列表的数量，不代表当前在途输出块使用的来源数量。
/// @return 查询时受控制锁保护的来源条目数。
/// @warning 获取控制锁；仅供控制和诊断路径查询。
std::size_t MixBus::sourceCount() const
{
    std::lock_guard<std::mutex> lock(m_controlMutex);
    return m_controlSources.size();
}

/// @brief 返回待回收版本数，用于观察控制发布与音频读取的交叠。
/// @return 快照版本数，而非音频节点数或尚在播放的来源数。
/// @warning 获取控制锁；非零数量本身不表示泄漏，受保护版本必须暂留。
std::size_t MixBus::retiredSnapshotCount() const
{
    std::lock_guard<std::mutex> lock(m_controlMutex);
    return m_retiredSnapshots.size();
}

/// @brief 返回无需分块的处理上限，未预备时返回零。
/// @return 以采样帧为单位的容量；不是字节数，也不乘声道数量。
/// @warning 不与 prepare 并发；此查询不会锁定格式或容量状态。
std::size_t MixBus::maxPreparedFrames() const
{
    return m_isPrepared ? m_maxPreparedFrames : 0U;
}

/// @brief 读取超容量分块的累计诊断值，不提供其他处理状态的同步保证。
/// @warning relaxed 读取只用于观察，不能作为音频工作完成的栅栏。
std::uint64_t MixBus::oversizedProcessCount() const
{
    return m_oversizedProcessCount.load(std::memory_order_relaxed);
}

/// @brief 读取处理失败次数；一次失败输出块只计一次。
/// @warning relaxed 读取不发布缓冲内容，也不能代替回调停止协议。
std::uint64_t MixBus::rejectedProcessCount() const
{
    return m_rejectedProcessCount.load(std::memory_order_relaxed);
}

/// @brief 在控制锁内完成新快照构造、地址发布和旧版本回收。
/// @pre 调用者已完成控制列表修改，并持续持有 m_controlMutex。
/// @warning 共享所有权复制和分配仅发生在来源变更时，不得移入音频回调。
void MixBus::publishSourceSnapshotLocked()
{
    // 完整复制后再发布地址，读取者不会看到构造到一半的来源列表。
    auto nextSnapshot                 = std::make_unique<SourceSnapshot>();
    nextSnapshot->sources             = m_controlSources;
    const SourceSnapshot* nextAddress = nextSnapshot.get();

    if ( m_activeSnapshotOwner ) {
        // 旧地址可能正在被音频线程保护；先转入退休列表，不立即析构。
        m_retiredSnapshots.push_back(std::move(m_activeSnapshotOwner));
    }
    m_activeSnapshotOwner = std::move(nextSnapshot);
    // 与 hazard 发布及二次校验共享顺序一致序列，不能单独放宽此处内存序。
    // 新版本的所有权已就绪，音频线程只需要借用地址。
    m_activeSnapshot.store(nextAddress, std::memory_order_seq_cst);
    reclaimRetiredSourcesLocked();
}

/// @brief 仅释放单一音频读取者未保护的历史快照。
/// @pre 不与其他控制侧发布/回收操作并行，音频线程遵守先保护后解引用协议。
/// @warning 必须持有控制锁；该锁防止并发发布修改退休列表。
void MixBus::reclaimRetiredSourcesLocked()
{
    // 当前版本由独占成员持有，不在退休列表里；这里只检查旧版本。
    // 一个 hazard 槽对应一个不可重入的音频读取者，不能直接扩展为多读取者。
    const SourceSnapshot* protectedSnapshot =
        m_hazardSnapshot.load(std::memory_order_seq_cst);
    // 回调若尚未完成保护校验，就不能解引用快照；若已校验，则保护地址需保留。
    // 析构发生在控制线程，避免最后一个节点引用在实时线程触发复杂释放。
    std::erase_if(
        m_retiredSnapshots,
        [protectedSnapshot](const std::unique_ptr<SourceSnapshot>& snapshot) {
            return snapshot.get() != protectedSnapshot;
        });
}

/// @brief 发布读取保护并二次确认来源地址，返回可解引用的稳定版本。
/// @return 借用快照指针，仅在调用 releaseSourceSnapshot 前有效。
/// @warning 每个输出块调用；单读取者 hazard 协议，不分配、不加锁。
/// 控制侧持续发布时可能重试；不是带固定等待时长的同步，也不保证固定重试次数。
const MixBus::SourceSnapshot* MixBus::acquireSourceSnapshot() noexcept
{
    const SourceSnapshot* snapshot{ nullptr };
    do {
        // 第一次加载仅取得候选地址，此时还不能读取其中的来源列表。
        snapshot = m_activeSnapshot.load(std::memory_order_seq_cst);
        m_hazardSnapshot.store(snapshot, std::memory_order_seq_cst);
        // 保护发布期间若活动版本发生变化，则重新保护新候选版本。
        // 只有二次加载相同才可离开循环；控制侧回收依赖同一顺序一致协议。
    } while ( snapshot != m_activeSnapshot.load(std::memory_order_seq_cst) );
    return snapshot;
}

/// @brief 结束读取保护，允许之后的控制侧操作回收旧版本。
/// @warning 每个输出块及失败退出路径调用；仅写原子标志，不执行析构。
void MixBus::releaseSourceSnapshot() noexcept
{
    // 清空后禁止继续访问刚才的快照；退休列表的实际回收留给控制线程。
    m_hazardSnapshot.store(nullptr, std::memory_order_seq_cst);
}

/// @brief 先应用左右声道路由，再从最终输出更新显示峰值。
/// @param buffer 已完成累加或已清零的输出块，声道路由可能就地覆盖样本。
/// @warning 音频周期热路径及失败出口都会调用，不得加锁或分配。
/// 电平仅是 UI 近似观测值，独立原子读写不提供成对采样的一致性。
void MixBus::finalizeOutput(AudioBuffer& buffer) noexcept
{
    float** samples = buffer.raw_ptrs();
    if ( samples ) {
        const std::size_t frameCount = buffer.num_frames();
        // 每块读取一次路由模式，避免同一个块的左右声道采用不同配置。
        // 这些模式只处理前两个声道，其余声道保持原混音结果。
        switch ( get_channel_mode() ) {
        case MixBusChannelMode::MuteLeft:
            if ( buffer.num_channels() > 0U ) {
                std::memset(samples[0], 0, frameCount * sizeof(float));
            }
            break;
        case MixBusChannelMode::MuteRight:
            if ( buffer.num_channels() > 1U ) {
                std::memset(samples[1], 0, frameCount * sizeof(float));
            }
            break;
        case MixBusChannelMode::CopyLeftToRight:
            // 复制左右声道必须实际存在双声道；单声道不能访问第二平面。
            if ( buffer.num_channels() > 1U ) {
                std::memcpy(samples[1], samples[0], frameCount * sizeof(float));
            }
            break;
        case MixBusChannelMode::CopyRightToLeft:
            if ( buffer.num_channels() > 1U ) {
                std::memcpy(samples[0], samples[1], frameCount * sizeof(float));
            }
            break;
        case MixBusChannelMode::Stereo: break;
        }
    }

    // 路由之后再测量，电平才能反映静音/复制后的实际输出，而非原始来源。
    float maxLeft{ 0.0F };
    float maxRight{ 0.0F };
    if ( samples ) {
        // 空指针或零活动帧不会访问样本；默认零峰值仍会进入后面的衰减更新。
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
            // 单声道在两个显示表头上呈现同一个峰值，不制造虚假的右声道音频。
            maxRight = maxLeft;
        }
    }

    // 峰值上升立即显示，下降按每次调用衰减；这不是按真实时间计算的包络。
    // 空缓冲和失败静音也经过此处，因此电平不会永久停在最后一个非零值。
    // UI 可独立读取左右值；relaxed 足够，不用于传递音频缓冲或来源所有权。
    const float previousLeft  = m_leftLevel.load(std::memory_order_relaxed);
    const float previousRight = m_rightLevel.load(std::memory_order_relaxed);
    m_leftLevel.store(maxLeft > previousLeft ? maxLeft : previousLeft * 0.95F,
                      std::memory_order_relaxed);
    m_rightLevel.store(
        maxRight > previousRight ? maxRight : previousRight * 0.95F,
        std::memory_order_relaxed);
}
}  // namespace ice
