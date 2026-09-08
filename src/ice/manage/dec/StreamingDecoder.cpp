#include "ice/manage/dec/StreamingDecoder.hpp"

#include "ice/manage/AudioBuffer.hpp"
#include "ice/manage/AudioFormat.hpp"
#include "ice/manage/dec/IDecoderFactory.hpp"
#include "ice/manage/dec/IDecoderInstance.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <limits>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

namespace ice
{
/// @brief 固定大小的页缓存与预读请求队列，IO 始终在缓存锁外执行。
/// 每个流式音轨拥有独立实例，避免不同文件之间串行等待磁盘读取。
/// 音轨内部多个播放位置复用缓存，但不会共享上层播放游标。
/// 固定页数同时约束 PCM 容量、请求存储和每次缓存查找的工作量。
/// 工作线程的 scratch 是唯一额外页，不随已读取时长累积数据。
/// 缓存锁保护页地址和描述符的整体一致性，不负责后端实例同步。
struct StreamingDecoder::State {
    /// @brief 每页约为 48 kHz 下的三分之一秒，兼顾小块读取和定位成本。
    static constexpr size_t PAGE_FRAMES = 16384;
    /// @brief 最多保留约五秒立体声 PCM，多个播放位置共享有界缓存。
    static constexpr size_t PAGE_COUNT = 16;
    /// @brief 未使用页及空请求的哨兵，不作为有效起始帧。
    static constexpr size_t EMPTY = std::numeric_limits<size_t>::max();
    /// @brief 一页稳定存储；读写均受缓存互斥量保护。
    struct Page {
        /// @brief 源目标帧起点，EMPTY 表示从未发布。
        size_t m_start{ EMPTY };
        /// @brief 有效前缀，文件尾部可以小于页容量。
        size_t m_frames{ 0 };
        /// @brief 创建时分配，工作线程以交换缓冲发布结果。
        AudioBuffer m_pcm;
    };
    /// @brief 仅由工作线程顺序访问的后端实例。
    std::unique_ptr<IDecoderInstance> m_instance;
    /// @brief 供首块准备和后台读取复用的输出格式。
    AudioDataFormat m_format;
    /// @brief 缓存页数量固定，回调遍历成本与音频长度无关。
    std::array<Page, PAGE_COUNT> m_pages;
    /// @brief 已分配页数，短文件不强行占用长音频的缓存预算。
    size_t m_pageCount{ PAGE_COUNT };
    /// @brief 缓存锁不覆盖 IO；回调只用 try_lock，竞争时输出静音。
    std::mutex m_mutex;
    /// @brief 有界请求环；同一页只保留一次未处理请求。
    std::array<size_t, PAGE_COUNT> m_requests;
    /// @brief 请求环读位置，由缓存锁保护。
    size_t m_head{ 0 };
    /// @brief 请求环中待处理数量，由缓存锁保护。
    size_t m_count{ 0 };
    /// @brief 正在解码的页，受缓存锁保护，避免消费者重复排队导致无谓倒退。
    size_t m_inFlight{ EMPTY };
    /// @brief 下一被替换页，顺序淘汰避免热路径排序。
    size_t m_victim{ 0 };
    /// @brief 当前后端的精确目标帧游标，仅工作线程修改。
    size_t m_cursor{ 0 };
    /// @brief 工作线程发布真实 EOF，读取侧仅取快照以裁剪请求。
    /// @warning 每块 relaxed
    /// 读取独立长度，不承担缓存页的发布；页内容由互斥量保护。
    std::atomic<size_t> m_total{ 0 };
    /// @brief 通知版本由请求侧递增，工作线程等待版本变化。
    /// @warning 每批请求 release 发布，后台 acquire
    /// 等待，避免通知先于等待而丢失。
    std::atomic<size_t> m_wake{ 0 };
    /// @brief 析构侧写入停止，后台在每次分块 IO 之间以 relaxed 检查。
    /// 停止只承载独立布尔值；进入等待前由 wake 的 release/acquire
    /// 保证不丢退出通知。
    std::atomic<bool> m_stopping{ false };
    /// @brief 声明在状态末尾，并在析构中先停止，保证引用状态仍然有效。
    std::thread m_worker;

    /// @brief 在已持锁情况下检查页是否驻留。
    /// @param start 已按 PAGE_FRAMES 对齐的目标帧。
    /// @return 页面已发布时返回 true，包括用于表示 EOF 的短页。
    /// @warning 每次分块读取调用，只扫描固定页表，不读取媒体元信息。
    bool contains(size_t start) const
    {
        return std::any_of(
            m_pages.begin(), m_pages.end(), [start](const Page& page) {
                return page.m_start == start;
            });
    }
    /// @brief 在已持锁情况下去重并排队；队列满时等待未来回调重新请求。
    /// @param start 需要驻留的页起点，不接收任意字节偏移。
    /// @warning 每音频块调用，可更新唤醒计数但不得分配请求节点或等待。
    void request(size_t start)
    {
        if ( start >= m_total.load(std::memory_order_relaxed) ||
             contains(start) || start == m_inFlight )
            return;
        // 重复回调可能在 IO 完成前多次请求同一页，去重防止其占满队列。
        // 不合并不同位置的请求，允许复合时间线同时消费同一音轨的远近片段。
        for ( size_t i = 0; i < m_count; ++i ) {
            if ( m_requests[(m_head + i) % PAGE_COUNT] == start ) return;
        }
        // 队列饱和不在音频线程扩容；当前请求保持静音，下个回调可再次提交。
        // 已经排队的请求不能被新预读覆盖，否则低频播放位置可能永远饥饿。
        if ( m_count == PAGE_COUNT ) return;
        m_requests[(m_head + m_count++) % PAGE_COUNT] = start;
        // 队列由互斥量发布；版本用于把一次通知持续保存到消费者开始等待。
        // notify 本身不携带状态，不能只依赖通知而省略计数递增。
        m_wake.fetch_add(1, std::memory_order_release);
        m_wake.notify_one();
    }
    /// @brief 后台按请求解码页，精确跳转通过从头顺序丢弃样本实现。
    /// 后端 seek 只保证关键帧定位，不能直接把任意 seek 后样本当作目标帧。
    /// @warning 非实时线程允许 IO；倒退及远距离定位成本与跳过长度有关。
    void run()
    {
        // 分配只发生一次，后续页发布交换 scratch 与被淘汰页的存储。
        // 后端可以在内部维护压缩包缓存，但不会得到整文件大小的输出请求。
        AudioBuffer scratch(m_format, PAGE_FRAMES);
        while ( !m_stopping.load(std::memory_order_relaxed) ) {
            const auto version = m_wake.load(std::memory_order_acquire);
            size_t     target  = EMPTY;
            {
                std::lock_guard lock(m_mutex);
                // 前一请求已结束；同一把锁内接管下一请求，去重状态没有空窗。
                m_inFlight = EMPTY;
                if ( m_count != 0 ) {
                    target     = m_requests[m_head];
                    m_inFlight = target;
                    m_head     = (m_head + 1) % PAGE_COUNT;
                    --m_count;
                }
            }
            // 版本在读队列前取得，生产者在两者之间通知也不会丢失唤醒。
            if ( target == EMPTY ) {
                if ( m_stopping.load(std::memory_order_relaxed) ) break;
                m_wake.wait(version, std::memory_order_acquire);
                continue;
            }
            // 已消费游标之后的请求可以直接顺序推进，不重启重采样器。
            // 倒退请求不能复用旧滤波历史，因此必须明确从文件头重置。
            if ( target < m_cursor ) {
                // seek 失败时不谎报页已就绪，缓存中其他页仍然可用。
                // 不用忙等重试后端错误；后续消费者可再次提交请求。
                if ( !m_instance->seek(0) ) continue;
                m_cursor = 0;
            }
            // 分块丢弃保持有界内存，并允许析构在块间取消长距离跳转。
            while ( m_cursor < target &&
                    !m_stopping.load(std::memory_order_relaxed) ) {
                const size_t wanted = std::min(PAGE_FRAMES, target - m_cursor);
                const size_t got = m_instance->read(scratch.raw_ptrs(), wanted);
                // 累加实际输出帧而非请求帧，保证短读不会造成后续位置偏移。
                m_cursor += got;
                if ( got < wanted ) {
                    m_total.store(m_cursor, std::memory_order_relaxed);
                    break;
                }
            }
            if ( m_stopping.load(std::memory_order_relaxed) ) break;
            // 跳过阶段提前遇到 EOF 时不能把 scratch 的旧内容发布成目标页。
            // 停止请求也只在块间检查，避免后台继续遍历整段长音频。
            if ( m_cursor != target ) continue;
            const size_t got =
                m_instance->read(scratch.raw_ptrs(), PAGE_FRAMES);
            m_cursor += got;
            if ( got < PAGE_FRAMES )
                m_total.store(m_cursor, std::memory_order_relaxed);
            {
                // 只交换预分配存储，锁内不做文件访问或整块 PCM 复制。
                std::lock_guard lock(m_mutex);
                auto&           page = m_pages[m_victim++ % m_pageCount];
                std::swap(page.m_pcm, scratch);
                page.m_start  = target;
                page.m_frames = got;
            }
        }
    }
};

/// @brief 默认构造只建立空句柄，工厂完成初始化后才能对外发布。
StreamingDecoder::StreamingDecoder() = default;
/// @brief 先终止工作线程，再允许唯一状态句柄释放后端和页存储。
/// @warning 仅资源回收时调用，join 可能等待当前解码或文件读取。
StreamingDecoder::~StreamingDecoder()
{
    if ( !m_state ) return;
    // 停止版本也必须递增，确保空队列上等待的线程可以退出。
    m_state->m_stopping.store(true, std::memory_order_relaxed);
    m_state->m_wake.fetch_add(1, std::memory_order_release);
    m_state->m_wake.notify_one();
    if ( m_state->m_worker.joinable() ) m_state->m_worker.join();
}

/// @brief 在非实时侧验证格式、准备首块并建立长期预读服务。
/// @return 不支持的格式、未知时长或空工厂返回空句柄。
/// 后端历史异常行为不在此包装成成功，调用方仍遵循工厂失败契约。
/// @warning 此处允许同步读取首块，禁止从回调延迟创建实例。
std::unique_ptr<StreamingDecoder> StreamingDecoder::create(
    std::string_view path, const AudioDataFormat& target_format, ThreadPool&,
    std::shared_ptr<IDecoderFactory> factory)
{
    if ( !factory || target_format.channels == 0 ||
         target_format.samplerate == 0 )
        return {};
    auto decoder     = std::make_unique<StreamingDecoder>();
    decoder->m_state = std::make_unique<State>();
    auto& state      = *decoder->m_state;
    state.m_instance = factory->create_instance(path, target_format);
    if ( !state.m_instance ) return {};
    state.m_format = state.m_instance->get_source_format();
    if ( state.m_format.channels != target_format.channels ||
         state.m_format.samplerate != target_format.samplerate )
        return {};
    // 元信息用于时间线长度；未知长度先拒绝，避免将零误报为可播放资源。
    state.m_total.store(state.m_instance->get_source_total_frames(),
                        std::memory_order_relaxed);
    if ( state.m_total.load(std::memory_order_relaxed) == 0 ) return {};
    // 与完整缓存使用同一初始化定位，避免 AAC 等格式的首读与 seek 后读
    // 对编码延迟采用不同处理，导致切换策略或倒退时 PCM 起点变化。
    if ( !state.m_instance->seek(0) ) return {};
    auto& first = state.m_pages[0];
    first.m_pcm.resize(state.m_format, State::PAGE_FRAMES);
    first.m_start = 0;
    first.m_frames =
        state.m_instance->read(first.m_pcm.raw_ptrs(), State::PAGE_FRAMES);
    state.m_cursor = first.m_frames;
    state.m_victim = 1;
    if ( first.m_frames < State::PAGE_FRAMES ) {
        // 短音频已经全部读完，无需创建永久线程或分配其余十五页。
        // 保持同一流式接口语义，但资源占用只相当于一页。
        state.m_total.store(first.m_frames, std::memory_order_relaxed);
        return decoder;
    }
    const auto estimatedPages =
        state.m_total.load(std::memory_order_relaxed) / State::PAGE_FRAMES + 1;
    state.m_pageCount =
        std::clamp(estimatedPages, size_t(2), State::PAGE_COUNT);
    for ( size_t index = 1; index < state.m_pageCount; ++index ) {
        state.m_pages[index].m_pcm.resize(state.m_format, State::PAGE_FRAMES);
    }
    // 首块准备后再发布实例，避免普通从头播放必然遇到缺页。
    // 线程启动建立状态初始化的可见性，无需为不变格式和页容量添加原子成员。
    // 后续读取不能改变输出格式，否则已分配平面的声道容量将失效。
    state.request(State::PAGE_FRAMES);
    state.m_worker = std::thread([&state] { state.run(); });
    return decoder;
}

/// @brief 获取长度快照，空对象安全地报告零长度。
/// @warning 每块 relaxed 读取独立数值，无等待；读取页样本仍必须取得缓存锁。
size_t StreamingDecoder::num_frames() const
{
    return m_state ? m_state->m_total.load(std::memory_order_relaxed) : 0;
}

/// @brief 按请求区间填充输出，静音也计入已推进的时间线帧数。
/// @param buffer 各声道容量至少为 frames 的调用方存储。
/// @param channels 目标声道数，多余声道保留本次清零结果。
/// @param start 目标采样率下的绝对帧，不改变任一播放节点的游标。
/// @param frames 请求长度，越过 EOF 的尾部由调用方处理。
/// @warning 每音频块执行；try_lock 失败立即返回，不在这里解码或申请内存。
size_t StreamingDecoder::decode(float** buffer, uint16_t channels, size_t start,
                                size_t frames)
{
    const auto total = num_frames();
    if ( !buffer || channels == 0 || frames == 0 || start >= total ) return 0;
    const auto wanted = std::min(frames, total - start);
    // 先清零，缓存竞争和缺页都保持连续时间推进，不重复上一块音频。
    for ( uint16_t channel = 0; channel < channels; ++channel ) {
        if ( !buffer[channel] ) return 0;
        std::fill_n(buffer[channel], wanted, 0.0F);
    }
    std::unique_lock lock(m_state->m_mutex, std::try_to_lock);
    if ( !lock.owns_lock() ) return wanted;
    // 每次循环最多跨一页，单次请求可以覆盖任意多个页边界。
    // 在整个复制区间保留同一把锁，工作线程不能替换正在借读的页地址。
    size_t copied = 0;
    while ( copied < wanted ) {
        const size_t position = start + copied;
        const size_t pageStart =
            position / State::PAGE_FRAMES * State::PAGE_FRAMES;
        const size_t offset = position - pageStart;
        const size_t length =
            std::min(wanted - copied, State::PAGE_FRAMES - offset);
        for ( const auto& page : m_state->m_pages ) {
            if ( page.m_start != pageStart || offset >= page.m_frames )
                continue;
            const size_t available = std::min(length, page.m_frames - offset);
            for ( uint16_t channel = 0;
                  channel < std::min(channels, m_state->m_format.channels);
                  ++channel ) {
                std::copy_n(page.m_pcm.raw_ptrs()[channel] + offset,
                            available,
                            buffer[channel] + copied);
            }
            break;
        }
        m_state->request(pageStart);
        // 每个消费位置预读下一页，支持相互独立的多个播放节点。
        if ( pageStart <= State::EMPTY - State::PAGE_FRAMES )
            m_state->request(pageStart + State::PAGE_FRAMES);
        copied += length;
    }
    return wanted;
}

/// @brief 明确拒绝稳定视图请求，防止调用方保留会被淘汰的 PCM 地址。
/// 流式播放与离线分析应分别持有不同策略的音轨，可共享源文件路径。
size_t StreamingDecoder::origin(std::vector<std::span<const float>>&, size_t,
                                size_t)
{
    // 不返回会被后台替换的页指针，完整视图由上层单独加载缓存音轨。
    return 0;
}
}  // namespace ice
