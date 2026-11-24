#include "ice/config/config.hpp"
#include "ice/manage/AudioFormat.hpp"
#include "ice/manage/AudioPool.hpp"
#include "ice/manage/AudioTrack.hpp"
#include "ice/manage/dec/IDecoderFactory.hpp"
#include "ice/manage/dec/IDecoderInstance.hpp"
#include "ice/manage/dec/MediaInfo.hpp"
#include "ice/thread/ThreadPool.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <future>
#include <memory>
#include <semaphore>
#include <string_view>
#include <thread>

namespace
{
/// @brief 只在测试夹具命名空间启用时间字面量。
using namespace std::chrono_literals;

/// @brief 流式首块大小，用于确保回收用例真的启动后台预读。
/// 回收验证需要等待 Worker 线程而非只清理短文件的 PCM 内存。
constexpr std::size_t PAGE_FRAMES{ 16384U };
/// @brief 仅测试同步超时，保证错误实现不会无限阻塞用例。
/// 测试必须在慢资源保持阻塞期间确认独立操作完成，阈值并非性能目标。
constexpr auto TEST_WAIT{ 2s };

/// @brief 让测试观察到后端进入指定步骤，再由测试控制何时继续。
struct ReadGate {
    /// @brief 由解码调用报告已进入慢读。
    std::binary_semaphore entered{ 0 };
    /// @brief 由测试解除阻塞。不能依赖实际存储设备延迟来制造窗口。
    std::binary_semaphore proceed{ 0 };
};

/// @brief 选择在哪一次读阻塞，分别制造同步首读或后台预读。
/// 三种策略由 mock 后端消费，无需生产代码插入测试开关。
enum class HoldRead {
    /// @brief 完整执行同步首读，供独立路径和新代次使用。
    None,
    /// @brief 阻塞同步首读，模拟进缓存前的慢 IO。
    First,
    /// @brief 阻塞后台预读，模拟退出 Worker 时等待尚未返回的 IO。
    Prefetch,
};

/// @brief 生产流式解码器所用的内存 PCM 后端，能按要求停止在某次读取。
class ControlledDecoder final : public ice::IDecoderInstance
{
public:
    /// @brief 目标格式按值持有，门闩在音轨结束后才销毁。
    /// @param format 输出的采样率与声道必须匹配引擎内部格式。
    /// @param gate 夹具为可控慢轨提供，正常轨道不会使用。
    /// @param hold 决定在哪次 read 落在同步门闩上。
    ControlledDecoder(ice::AudioDataFormat format, ReadGate& gate,
                      HoldRead hold)
        : m_format(format), m_gate(gate), m_hold(hold)
    {
    }

    /// @brief 测试只需要初始化的零帧定位，保持同步首读入口契约。
    /// @return 定位到开始时为真；本测试不实现随机定位。
    bool seek(std::size_t frame) override
    {
        if ( frame != 0U ) return false;
        // 保留已经执行的门闩次数。回到起点只影响 PCM 游标。
        m_cursor = 0U;
        return true;
    }

    /// @brief 根据门闩选择阻塞的读取，然后只写实际连续样本。
    /// @param output 真正由 ICE 准备的平面缓冲，不向夹具借用任何样本地址。
    /// @param frames 每声道可写的容量。
    /// @return 实际生成的帧数，虚拟曲尾之后返回零。
    std::size_t read(float** output, std::size_t frames) override
    {
        ++m_reads;
        // First 用例首块短读后结束，Prefetch 用例铺满首块才有真正的后台线程。
        const bool blocked = (m_hold == HoldRead::First && m_reads == 1U) ||
                             (m_hold == HoldRead::Prefetch && m_reads == 2U);
        if ( blocked ) {
            m_gate.entered.release();
            // 失败路径也由测试解除；全程仅测试后端等待，不影响正式代码。
            m_gate.proceed.acquire();
        }
        // 短轨只准备一个缓存页，避免把 fast 音轨本身的后台工作混入判定。
        // 直接生成 PCM 的成本与真实读取请求长度成正比，未写尾部由引擎处理。
        const auto count =
            std::min(frames, get_source_total_frames() - m_cursor);
        for ( std::size_t ch = 0; ch < m_format.channels; ++ch )
            std::fill_n(output[ch], count, 0.5F);
        m_cursor += count;
        return count;
    }

    /// @brief 直接提供 ICE 期望的目标格式，无需真实设备或重采样。
    /// @return 本实例独占的不可变格式，读期间不会改变。
    const ice::AudioDataFormat& get_source_format() const override
    {
        return m_format;
    }

    /// @brief 普通音轨只输出短块，回收用的轨道在首块后仍有数据。
    /// @details 预读用例需要完整首块才能启动真实 Decoder Worker。
    std::size_t get_source_total_frames() const override
    {
        return m_hold == HoldRead::Prefetch ? 2U * PAGE_FRAMES : 4U;
    }

private:
    /// @brief 本实例固定的输出平面数与采样率。
    ice::AudioDataFormat m_format;
    /// @brief 夹具持有的门闩；测试先释放门闩，再等待后台退出。
    ReadGate& m_gate;
    /// @brief 解码实例确定的阻塞策略。
    HoldRead m_hold;
    /// @brief 实际已写帧数，单一解码线程维护。
    std::size_t m_cursor{};
    /// @brief 解码次数包含同步首读，后台预读前已递增一次。
    std::size_t m_reads{};
};

/// @brief 第一次打开 slow 路径时安装门闩，重试可以独立完成。
class ControlledFactory final : public ice::IDecoderFactory
{
public:
    /// @brief 工厂借用整个用例期间有效的门闩。
    /// @param gate 慢轨抵达选定阶段后报告并等待的信号。
    /// @param hold 仅第一个 slow 音轨使用的阻塞阶段。
    ControlledFactory(ReadGate& gate, HoldRead hold)
        : m_gate(gate), m_hold(hold)
    {
    }

    /// @brief 返回满足音轨构造的目标格式，假路径不会打开磁盘文件。
    /// @param path 区分同路径竞争和独立音轨命中。
    /// @param info 调用方持有的新元信息，使用测试默认的无标签和无估值。
    bool probe(std::string_view path, ice::MediaInfo& info) const override
    {
        if ( path == "slow" ) {
            // 计数不发布任何 PCM；它只用于确认相同加载请求只探测一次。
            m_slowProbes.fetch_add(1U, std::memory_order_relaxed);
        }
        info.format = ice::ICEConfig::internal_format;
        return true;
    }

    /// @brief 返回独立读游标；同路径新一代加载不应被旧门闩卡住。
    /// @param path 第一个 slow 地址可阻塞，其余请求不使用这个门闩。
    /// @param format 每个独立实例应报告的输出格式。
    /// @return 使用测试 PCM 生成器的有效实例，仍由生产 AudioTrack 管理。
    std::unique_ptr<ice::IDecoderInstance> create_instance(
        std::string_view            path,
        const ice::AudioDataFormat& format) const override
    {
        const bool firstSlow =
            path == "slow" &&
            m_slowInstances.fetch_add(1U, std::memory_order_relaxed) == 0U;
        return std::make_unique<ControlledDecoder>(
            format, m_gate, firstSlow ? m_hold : HoldRead::None);
    }

    /// @brief 所有申请线程收尾后读取探测总数。
    unsigned slow_probes() const
    {
        return m_slowProbes.load(std::memory_order_relaxed);
    }

private:
    /// @brief 为第一次 slow 实例提供控制点。
    ReadGate& m_gate;
    /// @brief 固定阻塞模式；派生解码实例之间不共享进度。
    HoldRead m_hold;
    /// @brief 统计 slow 路径的实际探测次数，排除音轨池返回旧句柄的巧合。
    mutable std::atomic<unsigned> m_slowProbes{};
    /// @brief 只向初次实例授予门闩；跨线程不涉及其他成员的发布。
    mutable std::atomic<unsigned> m_slowInstances{};
};

/// @brief 将文件系统和解码后端保持在正式缓存路径内，同时让测试可控制解码。
struct Fixture {
    /// @brief 初始化一个已缓存的独立短音轨，作为慢操作期间的命中基线。
    /// @param hold 指明随后对 slow 路径的第一次加载如何阻塞。
    explicit Fixture(HoldRead hold)
        : factory(std::make_shared<ControlledFactory>(gate, hold))
        , pool(factory)
        , fast(load("fast"))
    {
    }

    /// @brief 回归测试始终经过真实流式策略，命中者取得播放期所有权。
    /// @param file 测试路径标识，不依赖音频资源目录或文件权限。
    /// @return 对已发布对象的强引用，使测试可以逐一比较缓存对象身份。
    std::shared_ptr<ice::AudioTrack> load(std::string_view file)
    {
        return pool.get_or_load(threads, file, ice::CachingStrategy::STREAMING)
            .lock();
    }

    /// @brief 音轨关闭后才可以销毁内存后端所借用的门闩。
    ReadGate gate;
    /// @brief 用例结束前持续持有工厂，可读取探测统计。
    std::shared_ptr<ControlledFactory> factory;
    /// @brief 音轨池的依赖，寿命覆盖先于其销毁的 pool。
    ice::ThreadPool threads{ 4 };
    /// @brief 被测缓存，使用生产索引与流式解码线程。
    ice::AudioPool pool;
    /// @brief 另一条轨保持外部强引用，release_unused 不应将它逐出。
    std::shared_ptr<ice::AudioTrack> fast;
};

/// @brief 慢首块尚未解码时独立路径必须命中，同时同一路径只探测一次。
/// 同路径两方应最终取得同一控制块。独立路径则须在慢轨解除门闩前完成。
bool cachedLookupDuringLoad()
{
    Fixture fixture(HoldRead::First);
    if ( !fixture.fast ) return false;
    auto slow =
        std::async(std::launch::async, [&] { return fixture.load("slow"); });
    // 即使异常实现未抵达门闩，先补送放行信号，避免 future 析构无限等待。
    if ( !fixture.gate.entered.try_acquire_for(TEST_WAIT) ) {
        fixture.gate.proceed.release();
        return false;
    }

    // 在真实首块确定阻塞之后再发起并发请求，排除先解码结束再命中的情况。
    auto duplicate =
        std::async(std::launch::async, [&] { return fixture.load("slow"); });
    // 命中任务使用与慢轨完全相同的索引和工厂，因此能观察到池级锁争用。
    auto hit =
        std::async(std::launch::async, [&] { return fixture.load("fast"); });
    const bool hitReady = hit.wait_for(TEST_WAIT) == std::future_status::ready;
    // 失效实现需要先解除阻塞，再取得全部 future，报告超时而不是卡住测试。
    fixture.gate.proceed.release();
    // 共享引用受检查函数持有，待加载的实际 probe 计数是独立的去重证据。
    const auto loaded = slow.get();
    return hitReady && hit.get() == fixture.fast && loaded &&
           duplicate.get() == loaded && fixture.factory->slow_probes() == 1U;
}

/// @brief 显式失效允许新一代立即建立，并拒绝旧加载覆盖已发布的新一代。
/// 路径和签名相同，因此测试只能由槽代次区别上一次与当前的缓存结果。
bool invalidateWhileLoading()
{
    Fixture fixture(HoldRead::First);
    auto    stale =
        std::async(std::launch::async, [&] { return fixture.load("slow"); });
    if ( !fixture.gate.entered.try_acquire_for(TEST_WAIT) ) {
        fixture.gate.proceed.release();
        return false;
    }

    // 旧加载未完成时失效应只摘索引，不等待尚无音轨可回收的槽。
    auto remove = std::async(std::launch::async,
                             [&] { fixture.pool.invalidate("slow"); });
    if ( remove.wait_for(TEST_WAIT) != std::future_status::ready ) {
        fixture.gate.proceed.release();
        return false;
    }
    // 第二次创建可在旧调用仍阻塞的情况下完成。若需等待旧调用，则超时。
    auto fresh =
        std::async(std::launch::async, [&] { return fixture.load("slow"); });
    const bool newReady =
        fresh.wait_for(TEST_WAIT) == std::future_status::ready;
    fixture.gate.proceed.release();
    // 等旧任务结束后再确认当前索引，避免误把短暂发布的 fresh 当作通过。
    const auto current = fresh.get();
    return newReady && current && !stale.get() &&
           fixture.load("slow") == current &&
           fixture.factory->slow_probes() == 2U;
}

/// @brief 回收线程等待读取结束时，另一条缓存音轨仍可立即取得。
/// @param sweep 为真执行批量回收，否则显式失效同一轨道。
/// @return 慢轨析构停驻在后台门闩时快轨仍命中，方可通过。
bool cachedLookupDuringRetirement(bool sweep)
{
    Fixture fixture(HoldRead::Prefetch);
    if ( !fixture.fast ) return false;
    // 用弱引用接收缓存结果，模拟播放节点已释放，池成为最后的所有者。
    const auto slow = fixture.pool.get_or_load(
        fixture.threads, "slow", ice::CachingStrategy::STREAMING);
    if ( !fixture.gate.entered.try_acquire_for(TEST_WAIT) ) {
        fixture.gate.proceed.release();
        return false;
    }

    // 这里只保留弱引用。强引用归零时说明析构已开始等待真实预读线程。
    auto retiring = std::async(std::launch::async, [&] {
        if ( sweep ) return fixture.pool.release_unused() == 1U;
        fixture.pool.invalidate("slow");
        return true;
    });
    // 让出测试线程的执行机会，等待进入真实析构而非猜测回收线程是否已启动。
    const auto deadline = std::chrono::steady_clock::now() + TEST_WAIT;
    while ( !slow.expired() && std::chrono::steady_clock::now() < deadline )
        std::this_thread::yield();

    // expired 证实强引用耗尽，future 未就绪证实该次回收仍在等待。
    // 单独确认这两个条件，避免只把一个尚未被调度的回收当作慢退役。
    const bool destructorEntered =
        slow.expired() && retiring.wait_for(0s) == std::future_status::timeout;
    auto hit =
        std::async(std::launch::async, [&] { return fixture.load("fast"); });
    const bool hitReady = hit.wait_for(TEST_WAIT) == std::future_status::ready;
    // 坚持在放行前观测查表完成，才能证明回收线程没有持有全局索引锁。
    fixture.gate.proceed.release();
    // 回收数量仍按策略生效。只有 slow 数据应该被 release_unused 选中。
    return retiring.get() && destructorEntered && hitReady &&
           hit.get() == fixture.fast;
}
}  // namespace

/// @brief 分别执行加载、失效代次及两种回收路径，返回码标明失败场景。
/// @return 1 表示独立命中或去重失败，2 表示旧代次发布问题；
/// 3 和 4 分别表示显式失效和批量回收影响了其它路径的命中。
int main()
{
    if ( !cachedLookupDuringLoad() ) return 1;
    if ( !invalidateWhileLoading() ) return 2;
    if ( !cachedLookupDuringRetirement(false) ) return 3;
    if ( !cachedLookupDuringRetirement(true) ) return 4;
    return 0;
}
