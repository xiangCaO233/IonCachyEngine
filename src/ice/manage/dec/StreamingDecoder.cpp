#include "ice/manage/dec/StreamingDecoder.hpp"

#include "ice/manage/AudioBuffer.hpp"
#include "ice/manage/AudioFormat.hpp"
#include "ice/manage/dec/IDecoderFactory.hpp"
#include "ice/manage/dec/IDecoderInstance.hpp"
#include <SDL3/SDL_thread.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

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
    /// @brief 创建阶段准备的后台临时页，仅由工作线程使用并与缓存页交换存储。
    AudioBuffer m_scratch;
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
    /// EMPTY 表示上次精确跳转失败或未读取到样本，须从头恢复位置。
    size_t m_cursor{ 0 };
    /// @brief 长度提示，由创建阶段初始化，后台按完整读取扩大。
    /// @warning 回调按块 relaxed 读取供时间线推进，PCM 仍由缓存锁发布。
    std::atomic<size_t> m_lengthHint{ 0 };
    /// @brief 已读取到的末尾；EMPTY 表示仍可在长度提示之外读取。
    /// @warning 创建侧和预读线程写入，音频回调按块 relaxed 读取。
    /// 只判断区间边界，不用于发布 PCM；只有无错误的空读才确认曲尾。
    std::atomic<size_t> m_end{ EMPTY };
    /// @brief 后端不可恢复读取失败，零表示尚未失败。
    /// @warning 预读线程发布，短读的设备回调或控制侧 relaxed 查询。
    /// 错误码是独立状态；已取得的 PCM 仍由缓存互斥量发布。
    std::atomic<int> m_error{ 0 };

    /// @brief 在页首及后台共用的声道指针表，创建阶段按固定声道数分配。
    std::vector<float*> m_writePlanes;

    /// @brief 读取结果，保留后端报错前实际写入的有效帧。
    struct ReadResult {
        size_t frames;  ///< 已累积的有效前缀。
        int    error;   ///< 最后一次读取报告的错误，或长度违约。
    };

    /// @brief 在已分配的页内累积短读；零帧且无错才表示源结束。
    /// @param output 足够容纳 target 帧的固定平面缓冲。
    /// @param target 本轮允许写入的最大帧数，不超过一页。
    /// @warning 仅初始化及预读线程调用；音频回调不能进入后端读取。
    ReadResult fill(AudioBuffer& output, size_t target)
    {
        size_t filled = 0;
        while ( filled < target &&
                !m_stopping.load(std::memory_order_relaxed) ) {
            // 后端从每个平面开头写入；指针移到未写区，保持已有前缀。
            for ( size_t ch = 0; ch < m_writePlanes.size(); ++ch )
                m_writePlanes[ch] = output.raw_ptrs()[ch] + filled;
            const auto remaining = target - filled;
            const auto frames =
                m_instance->read(m_writePlanes.data(), remaining);
            const int error = m_instance->last_read_error();
            // 后端若违反容量约定，不把此次报告的样本计入前缀。
            if ( frames > remaining ) return { filled, -EIO };
            filled += frames;
            if ( error != 0 || frames == 0 ) return { filled, error };
        }
        return { filled, 0 };
    }
    /// @brief 通知版本由请求侧递增，工作线程等待版本变化。
    /// @warning 每批请求 release 发布，后台 acquire
    /// 等待，避免通知先于等待而丢失。
    std::atomic<size_t> m_wake{ 0 };
    /// @brief 析构侧写入停止，后台在每次分块 IO 之间以 relaxed 检查。
    /// 停止只承载独立布尔值；进入等待前由 wake 的 release/acquire
    /// 保证不丢退出通知。
    /// @warning 析构侧写入，预读线程在块间及等待前读取；不用于发布页内容。
    std::atomic<bool> m_stopping{ false };
    /// @brief 声明在状态末尾，并在析构中先停止，保证引用状态仍然有效。
    SDL_Thread* m_worker{ nullptr };

    /// @brief SDL 线程入口，只借用状态直至析构等待结束。
    /// @param userdata 已完成首块和页表准备的状态地址。
    /// @return 预读循环退出后返回零。
    /// @warning 后台入口包含 IO 与等待，状态必须存活到 SDL_WaitThread 返回。
    static int SDLCALL runWorker(void* userdata)
    {
        static_cast<State*>(userdata)->run();
        return 0;
    }

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
    /// @brief 运行期间持锁去重并排队；队列满时依赖未来回调重新请求。
    /// 创建阶段尚未启动工作线程，可在独占状态时调用，不要求虚设缓存锁。
    /// @param start 需要驻留的页起点，不接收任意字节偏移。
    /// @warning 每音频块调用，可更新唤醒计数但不得分配请求节点或等待。
    void request(size_t start)
    {
        if ( start >= m_end.load(std::memory_order_relaxed) ||
             m_error.load(std::memory_order_relaxed) != 0 || contains(start) ||
             start == m_inFlight )
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
    /// @brief 后台按请求解码页，仅在后端保证精确位置时跳过顺序读取。
    /// 普通 seek 只保证关键帧定位，因此作为从头重放的兼容入口。
    /// @warning 非实时线程允许 IO；倒退及远距离定位成本与跳过长度有关。
    void run()
    {
        // 创建阶段已准备临时页，后台入口不再分配这块音频存储。
        // 后端可以在内部维护压缩包缓存，但不会得到整文件大小的输出请求。
        auto& scratch = m_scratch;
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
            // 末尾可能在请求入队之后才确定，跳过已经确认越界的请求。
            if ( target >= m_end.load(std::memory_order_relaxed) ) continue;
            // 相邻预读沿用现有后端历史；远处缺页或回跳才尝试精确定位。
            // 空读不能用来确认远处 EOF，回到已知起点重走后才发布末尾。
            size_t got         = 0;
            bool   alreadyRead = false;
            int    error       = 0;
            if ( m_cursor != EMPTY &&
                 (target < m_cursor || target - m_cursor >= 2 * PAGE_FRAMES) ) {
                const auto positioned = m_instance->seek_exact(target);
                // 不支持时保留现有游标，向前仍可从上一次读取的位置继续。
                if ( positioned ) {
                    const auto wanted = std::min(PAGE_FRAMES, EMPTY - target);
                    if ( *positioned ) {
                        const auto decoded = fill(scratch, wanted);
                        got                = decoded.frames;
                        error              = decoded.error;
                        // 有有效前缀或明确错误时保留本次结果。无错误空读仍走
                        // 顺序恢复，避免把远处寻址误判成可证实的 EOF。
                        alreadyRead = got > 0 || error != 0;
                    }
                    m_cursor = alreadyRead ? target : EMPTY;
                }
            }
            if ( !alreadyRead && (m_cursor == EMPTY || target < m_cursor) ) {
                // 连已知起点都无法恢复时，将故障交给音源，禁止重复解码请求。
                m_cursor = EMPTY;
                if ( !m_instance->seek(0) ) {
                    m_error.store(-ESPIPE, std::memory_order_relaxed);
                    return;
                }
                m_cursor = 0;
            }
            // 分块丢弃保持有界内存，并允许析构在块间取消长距离跳转。
            while ( m_cursor < target &&
                    !m_stopping.load(std::memory_order_relaxed) ) {
                const size_t wanted  = std::min(PAGE_FRAMES, target - m_cursor);
                const auto   decoded = fill(scratch, wanted);
                // 跳过区间时没有目标页可交付，错误直接终止后台任务。
                if ( decoded.error != 0 ) {
                    m_error.store(decoded.error, std::memory_order_relaxed);
                    return;
                }
                // 仅使用实际读出的帧推进位置。
                m_cursor += decoded.frames;
                if ( m_stopping.load(std::memory_order_relaxed) ) break;
                if ( decoded.frames < wanted ) {
                    m_end.store(m_cursor, std::memory_order_relaxed);
                    break;
                }
            }
            if ( m_stopping.load(std::memory_order_relaxed) ) break;
            // 跳过阶段提前遇到 EOF 时不能把 scratch 的旧内容发布成目标页。
            // 停止请求也只在块间检查，避免后台继续遍历整段长音频。
            if ( m_cursor != target ) continue;
            // 最后一个可表示区间也受整数剩余范围限制，游标累加不能回绕。
            const size_t wanted = std::min(PAGE_FRAMES, EMPTY - m_cursor);
            if ( !alreadyRead ) {
                const auto decoded = fill(scratch, wanted);
                got                = decoded.frames;
                error              = decoded.error;
            }
            // 停止产生的部分写入属于析构取消，不能发布成曲尾。
            if ( m_stopping.load(std::memory_order_relaxed) ) break;
            m_cursor += got;
            {
                // 只交换预分配存储，锁内不做文件访问或整块 PCM 复制。
                std::lock_guard lock(m_mutex);
                auto&           page = m_pages[m_victim++ % m_pageCount];
                std::swap(page.m_pcm, scratch);
                page.m_start  = target;
                page.m_frames = got;
                // 先准备完整描述符，再公布新长度，避免新读者把尾页当成缺页。
                // 倒退缓存重建不会缩短已知前缀，成功短页已在 fill 中见到空读。
                if ( error != 0 ) {
                    // 先交付遇错前的有效样本，但绝不将失败写进正常曲尾。
                    m_lengthHint.store(
                        std::max(m_cursor,
                                 m_lengthHint.load(std::memory_order_relaxed)),
                        std::memory_order_relaxed);
                } else if ( got < PAGE_FRAMES ) {
                    m_end.store(m_cursor, std::memory_order_relaxed);
                } else if ( m_cursor >
                            m_lengthHint.load(std::memory_order_relaxed) ) {
                    m_lengthHint.store(m_cursor, std::memory_order_relaxed);
                }
            }
            if ( error != 0 ) {
                // 先放开缓存锁再公开终止状态。看到错误的控制侧仍可读取已交付前缀。
                m_error.store(error, std::memory_order_relaxed);
                return;
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
    // SDL 等待同时释放线程句柄；未启动或启动失败的空句柄无需等待。
    if ( m_state->m_worker ) SDL_WaitThread(m_state->m_worker, nullptr);
}

/// @brief 在非实时侧验证格式、准备首块并建立长期预读服务。
/// @return 无效路径、不支持的格式或空工厂返回空句柄。
/// 后端历史异常行为不在此包装成成功，调用方仍遵循工厂失败契约。
/// @warning 此处允许同步读取首块，禁止从回调延迟创建实例。
std::unique_ptr<StreamingDecoder> StreamingDecoder::create(
    std::string_view path, const AudioDataFormat& target_format, ThreadPool&,
    std::shared_ptr<IDecoderFactory> factory)
{
    // 工厂可能把路径交给 C 字符串接口，空字符不能导致实际打开路径被截断。
    // 路径拒绝先于状态分配和媒体访问，保持与完整缓存入口一致的输入边界。
    if ( path.empty() || path.find('\0') != std::string_view::npos ) return {};
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
    // 元信息只提供暂定长度及缓存预算。零估值仍需读取首块才能确定可用帧数。
    const size_t estimate = state.m_instance->get_source_total_frames();
    // 与完整缓存使用同一初始化定位，避免 AAC 等格式的首读与 seek 后读
    // 对编码延迟采用不同处理，导致切换策略或倒退时 PCM 起点变化。
    if ( !state.m_instance->seek(0) ) return {};
    auto& first = state.m_pages[0];
    // 缓冲拒绝尺寸时不能把空指针表交给后端读取。
    if ( !first.m_pcm.resize(state.m_format, State::PAGE_FRAMES) ) return {};
    first.m_start = 0;
    state.m_writePlanes.resize(state.m_format.channels);
    const auto initial = state.fill(first.m_pcm, State::PAGE_FRAMES);
    // 首块出现错误时拒绝建立音轨，避免调用者把损坏前缀当作正常短音频。
    if ( initial.error != 0 ) return {};
    first.m_frames = initial.frames;
    state.m_cursor = first.m_frames;
    state.m_victim = 1;
    if ( first.m_frames < State::PAGE_FRAMES ) {
        // 首个页未填满且遇到空读时按已结束处理，无需创建永久线程或分配其余十五页。
        // 无已知估值也可以是有效的短音频；此时只需保留实际读出的首块。
        state.m_end.store(first.m_frames, std::memory_order_relaxed);
        return decoder;
    }
    state.m_lengthHint.store(std::max(estimate, first.m_frames),
                             std::memory_order_relaxed);
    // 无估值时保留常规流式缓存容量；已知估值只用于预算，至少保留相邻两页。
    // 小于一页的真实短音频已经在上方返回，不会占用额外存储。
    const auto estimatedPages = estimate / State::PAGE_FRAMES + 1;
    state.m_pageCount =
        estimate == 0
            ? State::PAGE_COUNT
            : std::clamp(estimatedPages, size_t(2), State::PAGE_COUNT);
    for ( size_t index = 1; index < state.m_pageCount; ++index ) {
        // 只发布全部页存储准备成功的实例，失败由局部对象统一释放已分配页。
        if ( !state.m_pages[index].m_pcm.resize(state.m_format,
                                                State::PAGE_FRAMES) )
            return {};
    }
    // 短文件已提前返回，不为它额外分配后台临时页；长期预读必须在启动前就绪。
    if ( !state.m_scratch.resize(state.m_format, State::PAGE_FRAMES) )
        return {};
    // 首块准备后再发布实例，避免普通从头播放必然遇到缺页。
    // 线程启动建立状态初始化的可见性，无需为不变格式和页容量添加原子成员。
    // 后续读取不能改变输出格式，否则已分配平面的声道容量将失效。
    state.request(State::PAGE_FRAMES);
    state.m_worker =
        SDL_CreateThread(State::runWorker, "ICE stream reader", &state);
    // 创建失败没有工作线程借用状态，返回空值使局部对象统一释放后端和页缓存。
    if ( !state.m_worker ) return {};
    return decoder;
}

/// @brief 获取长度快照，空对象安全地报告零长度。
/// @warning 每块 relaxed 读取独立数值，无等待；读取页样本仍必须取得缓存锁。
size_t StreamingDecoder::num_frames() const
{
    if ( !m_state ) return 0;
    if ( const auto end = end_frame() ) return *end;
    return m_state->m_lengthHint.load(std::memory_order_relaxed);
}

/// @brief 返回确认的排除结束帧；未遇到无错空读时保留未知状态。
/// @warning 音频回调仅 relaxed 读取后台发布的独立边界，不依赖缓存锁。
std::optional<size_t> StreamingDecoder::end_frame() const noexcept
{
    if ( !m_state ) return 0;
    const auto end = m_state->m_end.load(std::memory_order_relaxed);
    return end == State::EMPTY ? std::nullopt : std::optional{ end };
}

/// @brief 获取后台读取或定位的终止原因，不把它合并为正常 EOF。
int StreamingDecoder::streaming_decode_error() const noexcept
{
    return m_state ? m_state->m_error.load(std::memory_order_relaxed) : 0;
}

/// @brief 返回从请求起点连续可读的前缀；缺页时等待后续回调重试。
/// @param buffer 各声道容量至少为 frames 的调用方存储。
/// @param channels 目标声道数，多余声道保留本次清零结果。
/// @param start 目标采样率下的绝对帧，不改变任一播放节点的游标。
/// @param frames 请求长度，越过 EOF 的尾部由调用方处理。
/// @warning 每音频块执行；try_lock 失败立即返回，不在这里解码或申请内存。
size_t StreamingDecoder::decode(float** buffer, uint16_t channels, size_t start,
                                size_t frames)
{
    if ( !m_state || !buffer || channels == 0 || frames == 0 ) return 0;
    const auto end = end_frame();
    if ( end && start >= *end ) return 0;
    const auto total =
        end ? *end : m_state->m_lengthHint.load(std::memory_order_relaxed);
    const auto wanted = start < total ? std::min(frames, total - start) : 0;
    // 先验证整张输出表，后续声道无效时也不能改写前面有效声道的样本。
    for ( uint16_t channel = 0; channel < channels; ++channel )
        if ( !buffer[channel] ) return 0;
    // 清零发生在验证之后；缺页或锁竞争时返回的静音不推进源游标。
    for ( uint16_t channel = 0; channel < channels; ++channel ) {
        std::fill_n(buffer[channel], wanted, 0.0F);
    }
    std::unique_lock lock(m_state->m_mutex, std::try_to_lock);
    // 未取得锁也无法修改请求环；保持游标，下个回调会重新请求同一位置。
    if ( !lock.owns_lock() ) return 0;
    if ( wanted == 0 ) {
        // 只有长度提示用完，尚未遇到实际末尾。请求目标页后暂停当前游标，
        // 避免把后台尚未确认存在的尾部推进成永久静音。
        m_state->request(start / State::PAGE_FRAMES * State::PAGE_FRAMES);
        return 0;
    }
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
        size_t available = 0;
        for ( const auto& page : m_state->m_pages ) {
            if ( page.m_start != pageStart || offset >= page.m_frames )
                continue;
            available = std::min(length, page.m_frames - offset);
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
        // 只消费连续前缀；跨页缺口后的采样由后续块从正确位置读取。
        copied += available;
        if ( available < length ) break;
    }
    return copied;
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
