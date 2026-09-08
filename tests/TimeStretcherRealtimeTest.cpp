/// @file
/// @brief 以合成上游验证变速节点的状态协议、输入预算和已捕获堆操作。
/// @details 不打开音频设备或媒体文件，避免设备时序与解码 IO 混入节点回归结果。
/// 测试使用显式失败计数而非 assert，发布构建定义 NDEBUG 后仍执行这些断言。
/// 当前场景采用双声道 48 kHz，不构成所有采样率、声道布局与设备块长的组合覆盖。
/// 堆操作计数不测量回调耗时、锁竞争或调度抖动，零计数不能证明满足实时截止期限。
/// 本文件定义全局分配替换入口，应独立链接，不能与另一组全局替换定义合并。
#include "ice/core/IAudioNode.hpp"
#include "ice/core/effect/TimeStretcher.hpp"
#include "ice/manage/AudioBuffer.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory>
#include <new>
#include <string_view>
#include <thread>

namespace
{

/// @brief 当前线程是否正在统计普通堆分配。
/// @details 只包围被测音频调用；夹具构造、诊断输出及其他线程不在该统计区间内。
/// 计数为零只证明已接入钩子的分配入口没有被当前线程调用。
thread_local bool g_trackAllocations{ false };

/// @brief 当前统计区间内的普通堆分配数量。
/// 仅记录次数，不记录地址或字节数，无法据此计算峰值占用和泄漏量。
thread_local std::size_t g_allocationCount{ 0U };

/// @brief 当前统计区间内的普通堆释放数量。
/// 区间外分配的对象可在区间内释放，不要求与本区间分配计数配平。
thread_local std::size_t g_deallocationCount{ 0U };

/// @brief 当前统计区间内的 C 堆分配调用数量。
/// malloc、calloc、realloc 共用此计数，不保留具体入口的细分结果。
thread_local std::size_t g_mallocCount{ 0U };

/// @brief 当前统计区间内的 C 堆释放调用数量。
/// 包含 realloc 的保守释放记录，不能与 free 实际回收次数等同。
thread_local std::size_t g_freeCount{ 0U };

/// @brief 测试时间线提供的 discontinuity 代际。
/// 代际是离散变化标记，不是音频帧游标，也不能换算为跳转距离。
struct TestTimelineEpoch {
    /// @brief 当前时间线 epoch。
    /// @warning 测试控制侧或信号源以 release 发布，音频查询侧以 acquire 读取。
    /// 原子仅模拟跨线程代际可见性，不保护夹具中的普通成员或延长夹具生命周期。
    std::atomic<std::uint64_t> generation{ 0U };
};

/// @brief 无分配的输入连续区间脚本。
/// @details 由测试线程预填后顺序消费，不支持并发添加或重放。
/// 固定存储避免 provider 自身的扩容污染实时分配计数。
/// 脚本只描述可拉取范围，不生成样本；对应信号节点必须由用例另行连接。
struct BoundaryScript {
    /// @brief 单个预设连续区间。
    struct Step {
        /// @brief 区间剩余输入帧数。
        /// 使用上游输入时间域，不是变速后的输出长度，也不包含声道倍数。
        std::size_t frameCount{ 0U };

        /// @brief 区间末端动作。
        ice::TimeStretcher::InputBoundary boundary{
            ice::TimeStretcher::InputBoundary::None
        };
    };

    /// @brief 添加一个测试区间。
    /// @param frameCount 区间帧数。
    /// @param boundary 区间末端动作。
    /// @pre 必须在消费前完成设置；超过八段时本实现静默忽略新增区间。
    void add(std::size_t frameCount, ice::TimeStretcher::InputBoundary boundary)
    {
        // 不扩容也不覆盖已设置区间；测试应自行保证脚本容量足够。
        if ( m_stepCount >= m_steps.size() ) return;
        m_steps[m_stepCount++] = {
            .frameCount = frameCount,
            .boundary   = boundary,
        };
    }

    /// @brief 返回下一段不超过 maxInputFrames 的连续区间。
    /// @param maxInputFrames 当前拉取上限。
    /// @return 连续区间及段尾动作。
    /// @warning 每次上游范围查询调用；不得引入分配、阻塞或日志输出。
    /// @details 脚本耗尽后表示无限连续输入，不隐含文件结束。
    /// 零预算不会消耗非零剩余区间，但当前位置的零帧步骤仍可返回边界。
    ice::TimeStretcher::InputSpan read(std::size_t maxInputFrames) noexcept
    {
        ++m_callbackCount;
        if ( m_stepIndex >= m_stepCount ) {
            // 未脚本化的尾部继续供数，让测试只控制需要观察的边界。
            return {
                .frameCount = maxInputFrames,
                .boundary   = ice::TimeStretcher::InputBoundary::None,
            };
        }

        Step& step = m_steps[m_stepIndex];
        // 消费会递减脚本中的帧数，保存的是剩余量，不是不可变的原始区间长度。
        if ( step.frameCount > maxInputFrames ) {
            // 小块读取不能提前触发段尾；只有消费完余量才返回原边界。
            step.frameCount -= maxInputFrames;
            return {
                .frameCount = maxInputFrames,
                .boundary   = ice::TimeStretcher::InputBoundary::None,
            };
        }

        // 零帧步骤也在此消费一次，可表示没有样本伴随的边界事件。
        const ice::TimeStretcher::InputSpan result{
            .frameCount = step.frameCount,
            .boundary   = step.boundary,
        };
        ++m_stepIndex;
        return result;
    }

    /// @brief 获取 provider 被查询的次数。
    /// @return 查询次数。
    /// 一个区间可被拆成多次查询，不能将此值当作实际跨越的边界数。
    [[nodiscard]] std::size_t callbackCount() const { return m_callbackCount; }

private:
    /// @brief 固定容量脚本。
    std::array<Step, 8U> m_steps{};

    /// @brief 有效脚本步数。
    std::size_t m_stepCount{ 0U };

    /// @brief 下一脚本索引。
    /// 耗尽后保持在已配置区间之后，默认连续输入不再推进索引。
    std::size_t m_stepIndex{ 0U };

    /// @brief provider 查询次数。
    std::size_t m_callbackCount{ 0U };
};

/// @brief 调用测试输入边界 provider。
/// @param context BoundaryScript。
/// @param maxInputFrames 当前拉取上限。
/// @return 连续区间查询结果。
/// @pre 非空 context 必须指向仍存活的 BoundaryScript，且仅由一个消费线程访问。
/// 类型擦除指针不携带运行时类型信息，非空但类型错误的上下文不属于容错范围。
/// @warning 输入拉取热路径：只转发固定存储查询，不得增加锁或临时容器。
ice::TimeStretcher::InputSpan readInputBoundary(
    void* context, std::size_t maxInputFrames) noexcept
{
    if ( !context ) {
        // 空上下文退化为连续输入，而不是拒绝此次拉取。
        return {
            .frameCount = maxInputFrames,
            .boundary   = ice::TimeStretcher::InputBoundary::None,
        };
    }
    return static_cast<BoundaryScript*>(context)->read(maxInputFrames);
}

/// @brief 从测试时间线上读取 epoch。
/// @param context 指向 TestTimelineEpoch。
/// @return 当前 epoch。
/// @pre 非空 context 的生命周期必须覆盖回调注册和所有音频查询。
/// @warning 音频处理中重复查询；acquire 与夹具的 release 发布配对，不执行等待。
std::uint64_t readTimelineEpoch(const void* context) noexcept
{
    // 空上下文与实际代际零都返回零，不能借此查询 provider 是否注册。
    if ( !context ) return 0U;
    const auto* epoch = static_cast<const TestTimelineEpoch*>(context);
    return epoch->generation.load(std::memory_order_acquire);
}

/// @brief 生成固定幅度锯齿波的无分配测试节点。
/// @details 波形相位跨块连续且各声道相同，用于观察历史输出是否被清除。
/// 原子计数和幅度不代表整个节点可并发 process；相位及一次性通知由单线程推进。
class SignalNode final : public ice::IAudioNode
{
public:
    /// @brief 写入一块测试音频。
    /// @param buffer 输出缓冲。
    /// @warning 测试音频热路径：不得分配内存。
    /// @warning 每次拉取以 relaxed 更新观测计数、读取控制侧幅度；这些原子只保证
    /// 单值访问不竞争，不发布样本。代际通知使用 release 供查询侧 acquire 观察。
    void process(ice::AudioBuffer& buffer) override
    {
        // 计数的是请求而非成功写入量，因此空样本指针也保留本次调用记录。
        m_callCount.fetch_add(1U, std::memory_order_relaxed);
        m_processedFrames.fetch_add(buffer.num_frames(),
                                    std::memory_order_relaxed);
        if ( m_epochToAdvance ) {
            // 在上游回调内部制造代际变化，覆盖下游拉取前后 epoch 不一致的窗口。
            m_epochToAdvance->generation.store(m_generationToPublish,
                                               std::memory_order_release);
            m_epochToAdvance = nullptr;
        }
        // 一个块只读取一次幅度，避免控制侧更新导致同块不同声道幅度不一致。
        const float amplitude = m_amplitude.load(std::memory_order_relaxed);
        float**     samples   = buffer.raw_ptrs();
        if ( !samples ) return;

        for ( std::uint16_t channel = 0U; channel < buffer.num_channels();
              ++channel ) {
            for ( std::size_t frame = 0U; frame < buffer.num_frames();
                  ++frame ) {
                const float phase =
                    static_cast<float>((m_frame + frame) % 32U) / 31.0F;
                // 短周期确定信号用于观察清尾，不是抗混叠音质参考源。
                samples[channel][frame] = amplitude * (phase * 2.0F - 1.0F);
            }
        }
        // 所有声道共用同一相位原点，按块而非按声道推进。
        m_frame += buffer.num_frames();
    }

    /// @brief 设置后续输出幅度。
    /// @param amplitude 新幅度。
    /// @pre 提供有限幅度；夹具不做限幅，也不把非法值改为静音。
    /// @details relaxed 仅传递独立标量；不能作为其他夹具状态已就绪的同步信号。
    void setAmplitude(float amplitude)
    {
        m_amplitude.store(amplitude, std::memory_order_relaxed);
    }

    /// @brief 获取被拉取次数。
    /// @return process 调用次数。
    /// 并发观察可能早于该次样本写入，不能用计数增长代替处理完成的同步。
    [[nodiscard]] std::uint64_t callCount() const
    {
        return m_callCount.load(std::memory_order_relaxed);
    }

    /// @brief 获取上游累计提供的帧数。
    /// @return 所有 process 请求的逻辑帧数之和。
    /// 不乘声道数，与帧预算比较时不能换成总样本个数。
    [[nodiscard]] std::uint64_t processedFrames() const
    {
        return m_processedFrames.load(std::memory_order_relaxed);
    }

    /// @brief 让下一次上游拉取内部发布新的时间线 epoch。
    /// @param epoch 常驻测试时间线。
    /// @param generation 待发布代际。
    /// @pre 调用时不能与 process 并发；epoch 必须至少存活至下一次拉取结束。
    /// @details
    /// 只设置一次性通知，不立即改变代际；再次设置会覆盖尚未消费的通知。
    void advanceEpochDuringNextProcess(TestTimelineEpoch& epoch,
                                       std::uint64_t      generation)
    {
        m_epochToAdvance      = &epoch;
        m_generationToPublish = generation;
    }

private:
    /// @brief 当前输出幅度。
    /// @warning 控制侧写入、音频侧每块读取；独立标量使用
    /// relaxed，无跨成员同步含义。
    std::atomic<float> m_amplitude{ 0.5F };

    /// @brief 连续波形帧位置。
    /// 夹具没有 seek/reset 入口；下游重置不会回退此上游相位。
    std::size_t m_frame{ 0U };

    /// @brief 被上层拉取的次数。
    /// @warning 音频侧每次调用累加、测试侧读取；relaxed
    /// 计数不代表样本写入完成。
    std::atomic<std::uint64_t> m_callCount{ 0U };

    /// @brief 被上层请求的累计帧数。
    /// @warning 音频侧每次调用累加、测试侧读取；与调用计数不构成一致快照。
    std::atomic<std::uint64_t> m_processedFrames{ 0U };

    /// @brief 下一次 process 内需要更新的测试时间线。
    /// 指针清空表示通知已消费，不代表所指时间线对象可以立即在其他回调前销毁。
    TestTimelineEpoch* m_epochToAdvance{ nullptr };

    /// @brief 下一次 process 内发布的时间线代际。
    std::uint64_t m_generationToPublish{ 0U };
};

/// @brief 按拉取次数输出分段常量并记录每次请求大小。
/// @details 段是一次 process 调用而非固定帧数；正负常量用于区分边界两侧来源。
/// 除外部 epoch 外均为普通成员，配置、处理和结果检查必须串行执行。
class SegmentSignalNode final : public ice::IAudioNode
{
public:
    /// @brief 写入当前段常量值。
    /// @param buffer 输出缓冲。
    /// @warning 测试音频热路径：只写固定数组，不分配内存。
    /// @warning 首次拉取末尾可向测试 epoch 执行 release 发布，供下游 acquire
    /// 查询； 不得为构造此测试窗口增加 sleep、锁等待或跨线程握手。
    /// @pre buffer 的所有声道指针必须有效，本夹具不模拟缺失缓冲的失败路径。
    void process(ice::AudioBuffer& buffer) override
    {
        const std::size_t callIndex = m_callCount;
        if ( callIndex < m_requestedFrames.size() ) {
            // 只保存最初六十四次明细；累计请求数和帧数在容量耗尽后仍继续增长。
            m_requestedFrames[callIndex] = buffer.num_frames();
        }
        ++m_callCount;
        m_processedFrames += buffer.num_frames();

        // 零帧拉取也消耗一次调用序号，之后不会再次生成首段值。
        const float value   = callIndex == 0U ? m_firstValue : m_followingValue;
        float**     samples = buffer.raw_ptrs();
        for ( std::uint16_t channel = 0U; channel < buffer.num_channels();
              ++channel ) {
            std::fill_n(samples[channel], buffer.num_frames(), value);
        }

        if ( callIndex == 0U && m_epoch ) {
            // 先写旧段样本再发布代际，使下游必须处理已经拉到的旧时间线数据。
            m_epoch->generation.store(m_epochAfterFirstPull,
                                      std::memory_order_release);
        }
    }

    /// @brief 设置第一段与后续段的常量值。
    /// @param firstValue 第一段值。
    /// @param followingValue 后续段值。
    /// @details 不重置调用次数，修改 firstValue 不会重放已消费的首段。
    void setValues(float firstValue, float followingValue)
    {
        m_firstValue     = firstValue;
        m_followingValue = followingValue;
    }

    /// @brief 在第一次拉取结束时发布新 epoch。
    /// @param epoch 常驻测试 epoch。
    /// @param generation 新代际。
    /// @pre 应在首次 process 前设置；借用的 epoch 必须覆盖节点使用期间。
    /// @details 此方法不重置调用次数，首次拉取之后再设置不会补发通知。
    void publishEpochAfterFirstPull(TestTimelineEpoch& epoch,
                                    std::uint64_t      generation)
    {
        m_epoch               = &epoch;
        m_epochAfterFirstPull = generation;
    }

    /// @brief 获取拉取次数。
    /// @return process 调用次数。
    [[nodiscard]] std::size_t callCount() const { return m_callCount; }

    /// @brief 获取指定拉取的帧数。
    /// @param index 拉取索引。
    /// @return 对应帧数，越界返回零。
    /// @details 返回零也可能是合法零帧请求，不能据此区分越界与实际请求。
    [[nodiscard]] std::size_t requestedFrames(std::size_t index) const
    {
        return index < m_callCount && index < m_requestedFrames.size()
                   ? m_requestedFrames[index]
                   : 0U;
    }

    /// @brief 获取累计拉取帧数。
    /// @return 所有请求帧数之和。
    [[nodiscard]] std::size_t processedFrames() const
    {
        return m_processedFrames;
    }

private:
    /// @brief 每次拉取大小。
    /// 超过记录容量后不循环覆盖旧项，首次边界请求仍可用于失败诊断。
    std::array<std::size_t, 64U> m_requestedFrames{};

    /// @brief 拉取次数。
    std::size_t m_callCount{ 0U };

    /// @brief 累计拉取帧数。
    std::size_t m_processedFrames{ 0U };

    /// @brief 第一段输出值。
    float m_firstValue{ 0.6F };

    /// @brief 后续段输出值。
    float m_followingValue{ -0.6F };

    /// @brief 第一次拉取后更新的 epoch。
    TestTimelineEpoch* m_epoch{ nullptr };

    /// @brief 第一次拉取后发布的 epoch。
    std::uint64_t m_epochAfterFirstPull{ 0U };
};

/// @brief 检查测试条件。
/// @param condition 必须成立的条件。
/// @param label 失败标签。
/// @return 成立时返回 0，否则返回 1。
/// @pre 标签在诊断完成前有效且长度可表示为 int，当前调用均使用短字面量。
/// 不立即终止进程，使同一用例可累计多个独立不变量失败。
int expectTrue(bool condition, std::string_view label)
{
    if ( condition ) return 0;
    // 精度参数限制 string_view 输出长度，不依赖标签末尾有零终止符。
    std::fprintf(stderr,
                 "TimeStretcher realtime assertion failed: %.*s\n",
                 static_cast<int>(label.size()),
                 label.data());
    return 1;
}

/// @brief 检查缓冲是否为静音。
/// @param buffer 待检查缓冲。
/// @return 全部采样接近零时返回 true。
/// @details 空样本表也视为静音；本检查不是缓冲完整性或有限数检查。
/// NaN 与阈值的比较不会命中非静音分支，相关测试需单独检查非有限值。
/// 仅扫描活动帧，不扫描准备容量或 SIMD 填充区，不证明整个后端缓冲已清空。
/// @pre 非空样本表中的每个活动声道均需可读，缺失单声道指针不被视为静音。
bool isSilent(const ice::AudioBuffer& buffer)
{
    const float* const* samples = buffer.raw_ptrs();
    if ( !samples ) return true;
    for ( std::uint16_t channel = 0U; channel < buffer.num_channels();
          ++channel ) {
        for ( std::size_t frame = 0U; frame < buffer.num_frames(); ++frame ) {
            if ( std::abs(samples[channel][frame]) > 0.000001F ) return false;
        }
    }
    return true;
}

/// @brief 将首声道采样合并到跨 block 极值。
/// @param buffer 待观察缓冲。
/// @param minimum 当前最小值。
/// @param maximum 当前最大值。
/// @details 调用方初始化极值并跨块保留；仅观察首声道，不证明多声道输出一致。
/// 空输入不改输出参数，初始极值无法通过两侧幅度断言，避免无样本时空泛通过。
/// 不保留样本出现次序，两个极值都出现也不能证明边界先后顺序。
/// @pre minimum 与 maximum 必须是不同的存储位置，否则更新会相互覆盖。
void observeExtrema(const ice::AudioBuffer& buffer, float& minimum,
                    float& maximum)
{
    const float* const* samples = buffer.raw_ptrs();
    if ( !samples || buffer.num_channels() == 0U ) return;
    for ( std::size_t frame = 0U; frame < buffer.num_frames(); ++frame ) {
        minimum = std::min(minimum, samples[0][frame]);
        maximum = std::max(maximum, samples[0][frame]);
    }
}

/// @brief 统计首声道中可辨识的非静音帧。
/// @param buffer 待检查缓冲。
/// @return 绝对值超过测试阈值的帧数。
/// @details 统计的是阈值命中的首声道帧数，不是算法输出的有效帧数。
/// 合法零交叉及极小样本不会计入，因此仅适合这些已知测试信号的相对比较。
/// 不记录非静音帧的位置或连续性，不能证明中间没有缺帧。
/// NaN 不命中幅度阈值，而无穷大会命中；此计数不能替代音频有限性检查。
std::size_t countAudibleFrames(const ice::AudioBuffer& buffer)
{
    const float* const* samples = buffer.raw_ptrs();
    if ( !samples || buffer.num_channels() == 0U ) return 0U;

    std::size_t audibleFrames = 0U;
    for ( std::size_t frame = 0U; frame < buffer.num_frames(); ++frame ) {
        if ( std::abs(samples[0][frame]) > 0.000001F ) ++audibleFrames;
    }
    return audibleFrames;
}

/// @brief 清零计数器并开始统计当前线程堆操作。
/// @pre 统计窗口不能嵌套；再次调用会丢弃尚未检查的上一窗口计数。
/// @details 不统计其他线程；准备资源和失败诊断应放在窗口之外。
void beginHeapTracking()
{
    g_allocationCount   = 0U;
    g_deallocationCount = 0U;
    g_mallocCount       = 0U;
    g_freeCount         = 0U;
    // 最后打开开关；线程局部计数不需要跨线程原子同步，也不包含复位动作。
    g_trackAllocations = true;
}

/// @brief 停止统计当前线程堆操作。
/// @details 保留计数供后续断言读取，不触发回收也不等待任何后台工作。
void endHeapTracking()
{
    g_trackAllocations = false;
}

/// @brief 验证统计窗口没有任何 C++ 或 C 堆分配。
/// @details 仅检查本文件钩子实际捕获的入口，不能代替全进程分配追踪。
/// @pre 先结束统计窗口，防止失败诊断的运行库操作干扰被测计数。
/// @param label 失败标签。
/// @return 失败断言数量。
int expectNoHeapAllocation(std::string_view label)
{
    // 两类入口分别要求零，不相加解释，避免同次分配经过两层钩子时重复计数。
    if ( g_allocationCount == 0U && g_mallocCount == 0U ) return 0;
    std::fprintf(stderr,
                 "Heap allocation counters: new=%zu malloc=%zu\n",
                 g_allocationCount,
                 g_mallocCount);
    return expectTrue(false, label);
}

/// @brief 验证统计窗口没有任何 C++ 或 C 堆释放。
/// @details 释放与分配分别判定，以发现把旧状态析构移到音频回调中的退化。
/// @pre 先结束统计窗口；本断言不证明未接入钩子的释放入口没有被调用。
/// @param label 失败标签。
/// @return 失败断言数量。
int expectNoHeapDeallocation(std::string_view label)
{
    // 回调只释放而不分配也是退化，不能仅依靠分配断言发现这种情况。
    if ( g_deallocationCount == 0U && g_freeCount == 0U ) return 0;
    std::fprintf(stderr,
                 "Heap deallocation counters: delete=%zu free=%zu\n",
                 g_deallocationCount,
                 g_freeCount);
    return expectTrue(false, label);
}

/// @brief 验证预热后切速、状态切换与 reset 的回调零分配。
/// @details 本用例串行构造状态切换窗口，不等价于真实控制线程竞争测试。
/// 分配断言只读取 C++ 计数；统一统计助手也仅在启用链接包装时能观测 C 堆。
/// @return 失败断言数量。
int testRealtimeStateAndReset()
{
    constexpr ice::AudioDataFormat FORMAT{
        .channels   = 2U,
        .samplerate = 48000U,
    };
    constexpr std::size_t BLOCK_FRAMES = 256U;

    auto               source = std::make_shared<SignalNode>();
    ice::TimeStretcher stretcher;
    stretcher.set_inputnode(source);
    // 初始倍率和音高均偏离旁路条件，后续静音检查必须经过真实算法状态。
    stretcher.set_playback_ratio(0.75);
    stretcher.set_pitch_semitones(2.0);
    TestTimelineEpoch timelineEpoch;
    // 测试将时间线放在栈上，注册后一直保留至所有音频处理结束；回调不接管其所有权。
    stretcher.set_discontinuity_generation_provider(&timelineEpoch,
                                                    &readTimelineEpoch);
    const bool       prepared = stretcher.prepare(FORMAT, BLOCK_FRAMES);
    ice::AudioBuffer output(FORMAT, BLOCK_FRAMES);

    // prepare 和输出缓冲构造在窗口外；首次 process
    // 也纳入统计，不能靠回调内懒分配预热。
    // 连续三十二块使用同一输出存储，覆盖缓冲复用，而非每块重新构造的路径。
    g_allocationCount   = 0U;
    g_deallocationCount = 0U;
    g_trackAllocations  = true;
    for ( std::size_t block = 0U; block < 32U; ++block ) {
        stretcher.process(output);
    }
    g_trackAllocations = false;

    int failures = 0;
    failures += expectTrue(prepared, "prepare accepts valid format");
    // 返回准备失败仍会被计为测试失败，不能因后续保护路径无分配而掩盖准备错误。
    failures += expectTrue(g_allocationCount == 0U,
                           "steady callback performs no heap allocation");
    failures += expectTrue(g_deallocationCount == 0U,
                           "steady callback destroys no heap objects");
    failures += expectTrue(
        std::abs(stretcher.get_actual_playback_ratio() - 0.75) < 0.001,
        "requested playback ratio becomes active");

    // 控制侧更新不在窗口内，断言针对下一音频块消费参数的代价。
    // 先后两个 setter 可分别准备状态，音频侧只需接管最终待发布状态。
    // 不要求每个中间状态都曾被播放，也不把 getter 的变化当作可闻频率测量。
    stretcher.set_playback_ratio(1.5);
    stretcher.set_pitch_semitones(-3.0);
    g_allocationCount   = 0U;
    g_deallocationCount = 0U;
    g_trackAllocations  = true;
    stretcher.process(output);
    g_trackAllocations = false;
    failures += expectTrue(
        std::abs(stretcher.get_actual_playback_ratio() - 1.5) < 0.001,
        "live playback ratio switch becomes active");
    failures += expectTrue(g_allocationCount == 0U,
                           "live ratio switch allocates no heap memory");
    failures += expectTrue(g_deallocationCount == 0U,
                           "live ratio switch destroys no heap objects");

    // 把新输入置零后推进时间线，非零输出便可能暴露未清除的旧算法尾音。
    source->setAmplitude(0.0F);
    timelineEpoch.generation.store(1U, std::memory_order_release);
    // 外部时间线发布独立于 request_discontinuity，避免只测邮箱而漏掉 provider
    // 路径。
    g_allocationCount   = 0U;
    g_deallocationCount = 0U;
    g_trackAllocations  = true;
    stretcher.process(output);
    g_trackAllocations = false;
    failures +=
        expectTrue(stretcher.observed_provider_discontinuity_generation() == 1U,
                   "provider epoch is observed before pulling upstream");
    failures += expectTrue(g_allocationCount == 0U,
                           "provider reset allocates no heap memory");
    failures += expectTrue(g_deallocationCount == 0U,
                           "provider reset destroys no heap objects");
    failures += expectTrue(isSilent(output),
                           "provider epoch change removes previous tail");
    // 静音仅观察本次设备块；既有测试没有继续穷尽后端所有潜在缓冲尾音。

    // 先确认控制侧准备状态不会立即替换音频侧活动状态，再检查块边界接管。
    const std::uint64_t stateBefore = stretcher.active_state_generation();
    stretcher.set_quality(ice::TimeStretchQuality::Fast);
    failures += expectTrue(stretcher.active_state_generation() == stateBefore,
                           "prepared quality waits for block boundary");

    g_allocationCount   = 0U;
    g_deallocationCount = 0U;
    g_trackAllocations  = true;
    stretcher.process(output);
    g_trackAllocations = false;
    failures += expectTrue(stretcher.active_state_generation() > stateBefore,
                           "quality state switches at block boundary");
    // 使用严格增长而非加一，允许控制侧构造被后续发布覆盖的中间代际。
    // 代际变化只验证状态接管；不以 Fast 标签推导重建耗时或音质优劣。
    failures += expectTrue(g_allocationCount == 0U,
                           "state switch does not allocate in callback");
    failures += expectTrue(g_deallocationCount == 0U,
                           "state switch defers object destruction");

    // 显式重置请求与 provider
    // 代际是两条入口，分别观察其消费代际，避免混淆来源。
    const std::uint64_t resetGeneration = stretcher.request_discontinuity();
    // 用返回的实际代际作为期望值，不假定其他入口没有推进过内部状态。
    g_allocationCount   = 0U;
    g_deallocationCount = 0U;
    g_trackAllocations  = true;
    stretcher.process(output);
    g_trackAllocations = false;
    failures += expectTrue(
        stretcher.consumed_discontinuity_generation() == resetGeneration,
        "discontinuity is consumed at next block");
    failures += expectTrue(g_allocationCount == 0U,
                           "discontinuity reset does not allocate");
    failures += expectTrue(g_deallocationCount == 0U,
                           "discontinuity reset destroys no heap objects");
    failures +=
        expectTrue(isSilent(output), "discontinuity removes previous tail");

    // 重新输入非静音以建立历史，再让代际在上游 process 内变化而非块开始前变化。
    source->setAmplitude(0.5F);
    for ( std::size_t block = 0U; block < 4U; ++block ) {
        stretcher.process(output);
    }
    source->setAmplitude(0.0F);
    source->advanceEpochDuringNextProcess(timelineEpoch, 2U);
    // 通知只在下一次上游拉取执行；仅从输出缓存取数而不查询上游将无法观察到代际二。
    g_allocationCount   = 0U;
    g_deallocationCount = 0U;
    g_trackAllocations  = true;
    stretcher.process(output);
    g_trackAllocations = false;
    failures +=
        expectTrue(stretcher.observed_provider_discontinuity_generation() == 2U,
                   "upstream epoch change is consumed in the same callback");
    failures += expectTrue(g_allocationCount == 0U,
                           "intra-block provider reset does not allocate");
    failures += expectTrue(g_deallocationCount == 0U,
                           "intra-block provider reset does not destroy");
    failures +=
        expectTrue(isSilent(output), "intra-block reset removes previous tail");

    // 旧状态回收明确放在统计窗口之外；此处不为其控制线程开销设置性能断言。
    stretcher.collect_retired_states();
    return failures;
}

/// @brief 验证 final 请求只拉取最后一块并完整 drain。
/// @details 验证终态和上游调用次数，不比较逐采样结果或尾音实际长度。
/// 两百五十六块是测试失败上限，不是音频同步等待窗口，也不包含 sleep。
/// @return 失败断言数量。
int testFinalDrain()
{
    constexpr ice::AudioDataFormat FORMAT{
        .channels   = 2U,
        .samplerate = 48000U,
    };
    constexpr std::size_t BLOCK_FRAMES = 128U;

    auto               source = std::make_shared<SignalNode>();
    ice::TimeStretcher stretcher;
    stretcher.set_inputnode(source);
    stretcher.set_playback_ratio(0.8);
    stretcher.prepare(FORMAT, BLOCK_FRAMES);
    ice::AudioBuffer output(FORMAT, BLOCK_FRAMES);

    stretcher.process(output);
    const std::uint64_t callsBefore = source->callCount();
    // 正常处理一次后再终止，覆盖已有流状态的结束，而非空流直接封口。
    const std::uint64_t finalGeneration = stretcher.request_final_input();

    // 记录请求前已消费的输入，排尾期间只允许再拉取一次最终块。
    g_allocationCount   = 0U;
    g_deallocationCount = 0U;
    g_trackAllocations  = true;
    for ( std::size_t block = 0U;
          block < 256U && !stretcher.is_final_input_drained();
          ++block ) {
        stretcher.process(output);
    }
    g_trackAllocations = false;

    int failures = 0;
    failures +=
        expectTrue(stretcher.consumed_final_generation() == finalGeneration,
                   "final request is submitted");
    failures += expectTrue(stretcher.is_final_input_drained(),
                           "final output reaches drained state");
    // “请求已消费”和“尾部已排空”分别断言，不能用其中一个代替另一个终态条件。
    failures += expectTrue(source->callCount() == callsBefore + 1U,
                           "drain does not pull additional upstream blocks");
    failures += expectTrue(g_allocationCount == 0U,
                           "final and drain callbacks do not allocate");
    failures += expectTrue(g_deallocationCount == 0U,
                           "final and drain callbacks destroy no heap objects");
    return failures;
}

/// @brief 验证超过预备容量时安全静音且不临时扩容。
/// @details 只覆盖同格式下超出一帧的容量保护，不覆盖格式变化和整数极值。
/// @return 失败断言数量。
int testCapacityGuard()
{
    constexpr ice::AudioDataFormat FORMAT{
        .channels   = 2U,
        .samplerate = 48000U,
    };

    auto               source = std::make_shared<SignalNode>();
    ice::TimeStretcher stretcher;
    stretcher.set_inputnode(source);
    stretcher.set_playback_ratio(1.25);
    stretcher.prepare(FORMAT, 64U);
    // 上游不是空指针且倍率不走旁路，避免因其他保护条件先返回而误以为测到容量检查。
    // 在精确容量的下一帧触发保护，避免只测远超容量而遗漏边界比较错误。
    ice::AudioBuffer oversized(FORMAT, 65U);

    const std::uint64_t overflowsBefore = stretcher.capacity_overflow_count();
    // 比较前后差值，不假定构造或 prepare 期间计数器一定是零。
    g_allocationCount   = 0U;
    g_deallocationCount = 0U;
    g_trackAllocations  = true;
    stretcher.process(oversized);
    g_trackAllocations = false;

    int failures = 0;
    failures +=
        expectTrue(stretcher.capacity_overflow_count() == overflowsBefore + 1U,
                   "oversized block records capacity overflow");
    // 输出初始内容未设置非零哨兵，静音断言不能单独证明整个旧缓冲被覆盖。
    // 此用例依靠溢出计数增量确认实际命中了容量拒绝，而不只看输出值。
    failures +=
        expectTrue(isSilent(oversized), "oversized block is safely silenced");
    failures += expectTrue(g_allocationCount == 0U,
                           "oversized block never grows buffers in callback");
    failures += expectTrue(g_deallocationCount == 0U,
                           "oversized block destroys no heap objects");
    return failures;
}

/// @brief 验证暂停会冻结算法、上游拉取和待切换状态。
/// @details 通过调用数、活动代际及静音间接观察暂停，不直接访问算法内部游标。
/// 暂停与恢复在同一测试线程执行，不验证真实设备暂停或并发控制时序。
/// @return 失败断言数量。
int testPauseFreezesPipeline()
{
    constexpr ice::AudioDataFormat FORMAT{
        .channels   = 2U,
        .samplerate = 48000U,
    };
    constexpr std::size_t BLOCK_FRAMES = 128U;

    auto               source = std::make_shared<SignalNode>();
    ice::TimeStretcher stretcher;
    stretcher.set_inputnode(source);
    stretcher.set_playback_ratio(0.75);
    stretcher.prepare(FORMAT, BLOCK_FRAMES);
    ice::AudioBuffer output(FORMAT, BLOCK_FRAMES);
    stretcher.process(output);

    // 先暂停再准备新倍率，使暂停分支必须保留尚未接管的状态。
    stretcher.set_paused(true);
    stretcher.set_playback_ratio(1.25);
    const std::uint64_t callsBefore = source->callCount();
    const std::uint64_t stateBefore = stretcher.active_state_generation();

    beginHeapTracking();
    for ( std::size_t block = 0U; block < 8U; ++block ) {
        // 多次暂停处理检查持续冻结，不只检查刚设置暂停标志后的单个入口。
        stretcher.process(output);
    }
    endHeapTracking();

    int failures = 0;
    failures += expectTrue(stretcher.is_paused(), "pause request is visible");
    failures += expectTrue(source->callCount() == callsBefore,
                           "pause does not pull upstream");
    // 上游冻结和待发布状态冻结分别观察，输出静音本身无法区分后台是否仍在推进。
    failures += expectTrue(stretcher.active_state_generation() == stateBefore,
                           "pause does not consume pending state");
    failures += expectTrue(isSilent(output), "pause outputs silence");
    // 这里只检查最后一个暂停块的内容，计数窗口则覆盖所有八次调用。
    failures += expectNoHeapAllocation("pause callback does not allocate");
    failures += expectNoHeapDeallocation("pause callback does not free");

    // 恢复后的首块就应接管待发布状态并继续取数，不允许依赖额外空转块。
    stretcher.set_paused(false);
    beginHeapTracking();
    stretcher.process(output);
    endHeapTracking();
    failures += expectTrue(!stretcher.is_paused(), "resume request is visible");
    failures += expectTrue(stretcher.active_state_generation() > stateBefore,
                           "resume activates pending state at block boundary");
    failures += expectTrue(source->callCount() == callsBefore + 1U,
                           "resume continues upstream pull");
    // 调用数只验证取数恢复，尚未逐帧比较恢复后的信号与暂停前是否连续。
    failures += expectNoHeapAllocation("resume callback does not allocate");
    failures += expectNoHeapDeallocation("resume callback does not free");
    return failures;
}

/// @brief 验证输入帧小数余量避免长时间速度累计漂移。
/// @details 比较累计上游请求预算，而非输出音频时长、音高或听感。
/// 单次固定倍率的结果不能证明所有倍率变化组合均保持相同的舍入误差界。
/// @return 失败断言数量。
int testFractionalInputFrameRemainder()
{
    constexpr ice::AudioDataFormat FORMAT{
        .channels   = 2U,
        .samplerate = 48000U,
    };
    // 127 × 0.05 每块产生小数余量；1001 块可区分累计后取整与逐块丢弃余量。
    constexpr std::size_t BLOCK_FRAMES = 127U;
    constexpr std::size_t BLOCK_COUNT  = 1001U;
    constexpr double      RATIO        = 0.05;
    // 该场景每块预算仍大于一帧，不覆盖预算为零时 provider 事件的消费。

    auto               source = std::make_shared<SignalNode>();
    ice::TimeStretcher stretcher;
    stretcher.set_inputnode(source);
    stretcher.set_playback_ratio(RATIO);
    stretcher.prepare(FORMAT, BLOCK_FRAMES);
    ice::AudioBuffer output(FORMAT, BLOCK_FRAMES);

    beginHeapTracking();
    for ( std::size_t block = 0U; block < BLOCK_COUNT; ++block ) {
        stretcher.process(output);
    }
    endHeapTracking();

    // 参考值只在总预算上取整，不复用被测实现的逐块累加过程。
    const auto expectedInputFrames = static_cast<std::uint64_t>(
        std::floor(static_cast<double>(BLOCK_FRAMES * BLOCK_COUNT) * RATIO));

    int failures = 0;
    failures += expectTrue(source->processedFrames() == expectedInputFrames,
                           "fractional remainder preserves cumulative ratio");
    // 最终总数相等不能证明每块分配顺序完全正确；逐块预算分布不由此断言检查。
    failures +=
        expectNoHeapAllocation("fractional ratio callbacks do not allocate");
    failures +=
        expectNoHeapDeallocation("fractional ratio callbacks do not free");
    return failures;
}

/// @brief 验证极端倍率、音高及每次 reset 都保持固定容量和零堆操作。
/// @details
/// 四组参数交叉覆盖倍率端点、音高正负端点与四种质量，不是完整笛卡尔积。
/// 本用例不检查音频有限性、频谱或实际播放倍率，不能证明极端参数下的音质。
/// reset 分支只断言没有 C++ 释放，没有检查 C free 计数。
/// @return 失败断言数量。
int testExtremePreparedStates()
{
    constexpr ice::AudioDataFormat FORMAT{
        .channels   = 2U,
        .samplerate = 48000U,
    };
    constexpr std::size_t BLOCK_FRAMES = 256U;
    /// @brief 一组控制侧预备状态参数，不在音频回调内创建容器。
    struct ParameterCase {
        /// @brief 目标输入消耗倍率，与音高参数分别设置。
        double ratio;
        /// @brief 半音偏移，覆盖相反方向的变调状态。
        double semitones;
        /// @brief 本组状态的算法质量。
        ice::TimeStretchQuality quality;
    };
    constexpr ParameterCase CASES[]{
        { 0.05, -24.0, ice::TimeStretchQuality::Fast },
        { 10.0, 24.0, ice::TimeStretchQuality::Balanced },
        { 0.05, 24.0, ice::TimeStretchQuality::Finer },
        { 10.0, -24.0, ice::TimeStretchQuality::Best },
    };

    auto               source = std::make_shared<SignalNode>();
    ice::TimeStretcher stretcher;
    stretcher.set_inputnode(source);
    stretcher.prepare(FORMAT, BLOCK_FRAMES);
    ice::AudioBuffer output(FORMAT, BLOCK_FRAMES);

    int failures = 0;
    for ( const ParameterCase& parameterCase : CASES ) {
        // 参数设置可能准备并替换待发布状态，其控制侧分配不属于实时窗口。
        // 三个 setter
        // 顺序执行而非原子提交整组：每次准备可暂时组合新倍率与上一组音高/质量。
        // 因此后台警告不能只按 CASES 中最终组合归因，还需定位到具体准备调用。
        // 第三组先设置 0.05 时仍沿用前组 +24 半音和 Balanced，再更新后续参数。
        // 该中间状态不要求在音频侧接管，但其控制侧准备成本确实会发生。
        stretcher.set_playback_ratio(parameterCase.ratio);
        stretcher.set_pitch_semitones(parameterCase.semitones);
        stretcher.set_quality(parameterCase.quality);

        // 每组只观察首次接管块，不能代替该极端状态长时间连续处理的容量压力测试。
        // 准备阶段可扩容；统计窗口只覆盖接管与处理，不测量 setter
        // 的耗时或内存峰值。
        beginHeapTracking();
        stretcher.process(output);
        endHeapTracking();
        failures +=
            expectNoHeapAllocation("extreme state swap does not allocate");
        failures +=
            expectNoHeapDeallocation("extreme state swap does not free");

        // 第二个窗口与首次接管分开计数，以便失败标签区分状态切换和重置路径。
        const std::uint64_t resetGeneration = stretcher.request_discontinuity();
        // 接管后立即重置，覆盖恢复起始补偿时的缓冲使用，而非只观察持续运行。
        beginHeapTracking();
        stretcher.process(output);
        endHeapTracking();
        failures += expectTrue(
            stretcher.consumed_discontinuity_generation() == resetGeneration,
            "extreme state reset is consumed");
        // 此处代际相等证明请求被消费，不检查重置后输出是否含非有限值或旧波形。
        failures += expectNoHeapAllocation(
            "extreme reset and start padding do not allocate");
        failures +=
            expectTrue(g_deallocationCount == 0U,
                       "extreme reset does not destroy C++ heap objects");
    }

    stretcher.collect_retired_states();
    return failures;
}

/// @brief 验证 pending 参数不会截断 final 尾音且结束后不会恢复拉取。
/// @details 用活动代际检查未排空前不接管新状态，以调用计数检查终态不再取数。
/// 不逐采样比较尾音；终态标志正确并不单独证明尾音内容或长度完全正确。
/// @warning 排尾循环仅为有限次数的离线测试驱动，不能作为实时线程同步等待方案。
/// @return 失败断言数量。
int testPendingStateWaitsForFinalDrain()
{
    constexpr ice::AudioDataFormat FORMAT{
        .channels   = 2U,
        .samplerate = 48000U,
    };
    constexpr std::size_t BLOCK_FRAMES = 64U;

    auto               source = std::make_shared<SignalNode>();
    ice::TimeStretcher stretcher;
    stretcher.set_inputnode(source);
    stretcher.set_playback_ratio(0.5);
    stretcher.prepare(FORMAT, BLOCK_FRAMES);
    ice::AudioBuffer output(FORMAT, BLOCK_FRAMES);
    for ( std::size_t block = 0U; block < 8U; ++block ) {
        stretcher.process(output);
    }

    const std::uint64_t finalGeneration = stretcher.request_final_input();
    stretcher.process(output);
    const std::uint64_t drainingState = stretcher.active_state_generation();
    // 保存的是最终输入已提交后的活动状态，后续排尾必须继续使用这一算法历史。
    const std::uint64_t callsAfterFinalInput = source->callCount();

    // final
    // 已送入旧状态后才准备新参数，确保测试覆盖尾音排空与状态接管的先后约束。
    stretcher.set_playback_ratio(1.5);
    stretcher.set_pitch_semitones(7.0);
    stretcher.set_quality(ice::TimeStretchQuality::Best);

    beginHeapTracking();
    for ( std::size_t block = 0U;
          block < 2048U && !stretcher.is_final_input_drained();
          ++block ) {
        // 每次调用后检查不变量；最终变为 drained 的块允许发生后续状态接管。
        stretcher.process(output);
        if ( !stretcher.is_final_input_drained() &&
             stretcher.active_state_generation() != drainingState ) {
            // 先关闭计数再诊断，日志及局部对象析构不应算入实时窗口。
            endHeapTracking();
            return expectTrue(false,
                              "pending state replaced an undrained state");
        }
    }
    endHeapTracking();

    int failures = 0;
    failures +=
        expectTrue(stretcher.consumed_final_generation() == finalGeneration,
                   "pending-state final request is consumed");
    failures += expectTrue(stretcher.is_final_input_drained(),
                           "pending-state final tail is fully drained");
    failures += expectTrue(source->callCount() == callsAfterFinalInput,
                           "final drain does not pull upstream");
    failures += expectNoHeapAllocation("pending final drain does not allocate");
    failures += expectNoHeapDeallocation("pending final drain does not free");

    // 排空后再驱动一次，验证待发布状态不是永久被终态阻塞。
    // 允许接管新状态不等于允许重新读源，下面分别检查这两个状态机条件。
    beginHeapTracking();
    stretcher.process(output);
    endHeapTracking();
    failures +=
        expectTrue(stretcher.active_state_generation() > drainingState,
                   "pending state activates after tail reaches terminal state");
    failures += expectTrue(source->callCount() == callsAfterFinalInput,
                           "terminal replacement remains stopped");
    failures += expectTrue(stretcher.is_final_input_drained(),
                           "terminal replacement remains drained");
    // 代际增加与终态保持一起成立，才能排除仅因旧状态一直未替换而“不再拉取”。
    failures +=
        expectNoHeapAllocation("post-drain state activation does not allocate");
    failures +=
        expectNoHeapDeallocation("post-drain state activation does not free");

    // 排空后的新状态继承终止状态，只有显式跳变才应重新打开输入。
    // 这一末段不在分配统计窗口内，仅验证重新打开输入的行为。
    stretcher.request_discontinuity();
    stretcher.process(output);
    failures += expectTrue(source->callCount() == callsAfterFinalInput + 1U,
                           "discontinuity reopens terminal replacement");
    return failures;
}

/// @brief 验证 RubberBand realtime padding 与 delay 在每次 reset 后恢复。
/// @details 直接检查 RStretcher 延迟计数和带偏移写入，不经状态发布层。
/// 使用全零输入，不检验非静音起始瞬态或不同质量下的补偿长度。
/// reset 段只检查 C++ 释放计数，不能据此宣称 C 堆释放也为零。
/// @return 失败断言数量。
int testRealtimeStartCompensation()
{
    constexpr ice::AudioDataFormat FORMAT{
        .channels   = 2U,
        .samplerate = 48000U,
    };
    constexpr std::size_t BLOCK_FRAMES = 64U;

    // 输入、输出容量均为一个设备块；构造及缓冲分配位于计数窗口之外。
    // RStretcher 的 1.25 是输出时长倍率，不是 TimeStretcher 的输入消耗倍率。
    // 后一个 1.0 保持音高不变，使该用例聚焦起始延迟与偏移写入。
    ice::RStretcher  stretcher(FORMAT,
                               ice::TimeStretchQuality::Finer,
                               BLOCK_FRAMES,
                               BLOCK_FRAMES,
                               1.25,
                               1.0);
    ice::AudioBuffer input(FORMAT, BLOCK_FRAMES);
    ice::AudioBuffer output(FORMAT, BLOCK_FRAMES);
    input.clear();

    // 确认本配置确实存在补偿，避免零延迟路径让消费断言空泛通过。
    int failures = 0;
    failures += expectTrue(stretcher.preferred_start_pad() > 0U,
                           "realtime stretcher declares start padding");
    failures += expectTrue(stretcher.start_delay() > 0U,
                           "realtime stretcher declares start delay");
    // 预填输入帧与待丢弃输出帧不是同一种预算，不能要求两项数值相等。
    failures +=
        expectTrue(stretcher.remaining_start_delay() == stretcher.start_delay(),
                   "constructor leaves compensated stream ready");

    beginHeapTracking();
    for ( std::size_t block = 0U;
          block < 256U && stretcher.remaining_start_delay() > 0U;
          ++block ) {
        // false 表示持续流输入，不以 final 排尾捷径清除待补偿延迟。
        stretcher.process(output, input, false);
    }
    endHeapTracking();
    failures += expectTrue(stretcher.remaining_start_delay() == 0U,
                           "start delay is discarded across streaming calls");
    // 上限耗尽却仍有延迟会明确失败，不通过降低期望值接受未完成补偿。
    failures +=
        expectNoHeapAllocation("start-delay compensation does not allocate");
    failures +=
        expectNoHeapDeallocation("start-delay compensation does not free");

    // 前缀哨兵模拟同一设备块中已完成的前一段，后一段只能从指定偏移继续写入。
    constexpr std::size_t OUTPUT_PREFIX = 8U;
    float**               outputSamples = output.raw_ptrs();
    // 同时设置两声道前缀，避免只保护首声道而遗漏另一声道的偏移覆盖。
    for ( std::uint16_t channel = 0U; channel < output.num_channels();
          ++channel ) {
        for ( std::size_t frame = 0U; frame < OUTPUT_PREFIX; ++frame ) {
            outputSamples[channel][frame] = 0.375F;
        }
    }
    beginHeapTracking();
    stretcher.process_into(output, OUTPUT_PREFIX, input, false);
    // 调用只保留原指针借用，处理期间不得更换输出存储；此测试不主动 resize
    // 输出。
    endHeapTracking();
    // 只检查前缀未被覆盖，偏移后的样本内容不由此断言判断。
    bool prefixPreserved = true;
    for ( std::uint16_t channel = 0U; channel < output.num_channels();
          ++channel ) {
        for ( std::size_t frame = 0U; frame < OUTPUT_PREFIX; ++frame ) {
            prefixPreserved &=
                std::abs(outputSamples[channel][frame] - 0.375F) < 0.000001F;
        }
    }
    failures += expectTrue(prefixPreserved,
                           "segment output offset preserves prior samples");
    failures +=
        expectNoHeapAllocation("segment output offset does not allocate");
    failures += expectNoHeapDeallocation(
        "segment output offset does not free heap objects");

    beginHeapTracking();
    stretcher.reset();
    endHeapTracking();
    // reset 应恢复完整待丢弃预算，而不是沿用已经消费为零的延迟计数。
    failures +=
        expectTrue(stretcher.remaining_start_delay() == stretcher.start_delay(),
                   "reset restores full start-delay compensation");
    failures +=
        expectNoHeapAllocation("reset padding does not allocate C or C++ heap");
    failures += expectTrue(g_deallocationCount == 0U,
                           "reset padding does not delete C++ heap objects");
    return failures;
}

/// @brief 验证变速变调时同一设备 block 可跨越 loop 边界且不混用状态。
/// @details 精确检查首块的两段输入预算，再跨输出块观察两种极性。
/// 极值只能证明两侧信号曾出现，不证明边界逐采样连续或无瞬态混合。
/// 后续观察允许继续拉取，因此累计输入帧数断言是下界，而非精确长度。
/// @return 失败断言数量。
int testStretchedLoopBoundary()
{
    constexpr ice::AudioDataFormat FORMAT{
        .channels   = 2U,
        .samplerate = 48000U,
    };
    constexpr std::size_t BLOCK_FRAMES = 1024U;
    // 1024 × 0.8 的首块整数输入预算为 819，跳变位于预算内部而非块末尾。
    constexpr std::size_t RIGHT_SEGMENT     = 400U;
    constexpr std::size_t LEFT_SEGMENT      = 419U;
    constexpr std::size_t PLANNED_INPUT     = 819U;
    constexpr std::size_t MAX_OUTPUT_BLOCKS = 32U;

    auto source = std::make_shared<SegmentSignalNode>();
    source->setValues(0.6F, -0.6F);
    TestTimelineEpoch timelineEpoch;
    source->publishEpochAfterFirstPull(timelineEpoch, 1U);

    // 显式跳变与 epoch 发布表示同一边界，不能因此重复清除段内数据。
    BoundaryScript boundaries;
    boundaries.add(RIGHT_SEGMENT,
                   ice::TimeStretcher::InputBoundary::Discontinuity);
    boundaries.add(LEFT_SEGMENT, ice::TimeStretcher::InputBoundary::None);

    ice::TimeStretcher stretcher;
    stretcher.set_inputnode(source);
    stretcher.set_playback_ratio(0.8);
    stretcher.set_pitch_semitones(2.0);
    stretcher.set_discontinuity_generation_provider(&timelineEpoch,
                                                    &readTimelineEpoch);
    stretcher.set_input_boundary_provider(&boundaries, &readInputBoundary);
    const bool       prepared = stretcher.prepare(FORMAT, BLOCK_FRAMES);
    ice::AudioBuffer output(FORMAT, BLOCK_FRAMES);

    float minimum = std::numeric_limits<float>::max();
    float maximum = std::numeric_limits<float>::lowest();
    beginHeapTracking();
    stretcher.process(output);
    const std::size_t callsAfterBoundaryBlock = source->callCount();
    observeExtrema(output, minimum, maximum);
    // 算法延迟可能推迟左侧负信号出现；有限追加块用于观察，不要求当块全部输出。
    for ( std::size_t block = 1U; block < MAX_OUTPUT_BLOCKS && minimum > -0.3F;
          ++block ) {
        stretcher.process(output);
        observeExtrema(output, minimum, maximum);
    }
    endHeapTracking();

    int failures = 0;
    if ( callsAfterBoundaryBlock != 2U ||
         source->requestedFrames(1U) != LEFT_SEGMENT ) {
        // 保留首次两次请求的诊断，避免只有累计数而无法定位预算在哪一侧丢失。
        std::fprintf(
            stderr,
            "Loop diagnostics: first_block_calls=%zu calls=%zu "
            "request0=%zu request1=%zu processed=%zu callbacks=%zu "
            "epoch=%llu min=%f max=%f\n",
            callsAfterBoundaryBlock,
            source->callCount(),
            source->requestedFrames(0U),
            source->requestedFrames(1U),
            source->processedFrames(),
            boundaries.callbackCount(),
            static_cast<unsigned long long>(
                stretcher.observed_provider_discontinuity_generation()),
            static_cast<double>(minimum),
            static_cast<double>(maximum));
    }
    failures += expectTrue(prepared, "loop boundary state prepares");
    failures += expectTrue(callsAfterBoundaryBlock == 2U,
                           "one device block pulls both loop sides");
    // 必须同时核对两个请求大小，单凭两次调用无法排除把两侧预算互换或越过边界。
    failures += expectTrue(source->requestedFrames(0U) == RIGHT_SEGMENT,
                           "right loop side stops exactly at boundary");
    failures += expectTrue(source->requestedFrames(1U) == LEFT_SEGMENT,
                           "left loop side consumes remaining block budget");
    failures += expectTrue(source->processedFrames() >= PLANNED_INPUT,
                           "loop boundary preserves all planned input");
    failures +=
        expectTrue(stretcher.observed_provider_discontinuity_generation() == 1U,
                   "explicit boundary acknowledges matching timeline epoch");
    // 用幅度阈值区分正负两段而非比较波形相位，允许变调算法的合法瞬态形状变化。
    failures +=
        expectTrue(maximum > 0.3F, "right loop side produces audible output");
    failures +=
        expectTrue(minimum < -0.3F, "left loop side produces audible output");
    failures +=
        expectNoHeapAllocation("stretched loop callbacks do not allocate");
    failures += expectTrue(g_deallocationCount == 0U,
                           "stretched loop callbacks destroy no C++ objects");
    return failures;
}

/// @brief 验证 provider Final 精确限制最后输入并完整 drain。
/// @details 上游请求严格限定为三十七帧；可闻输出只检查非零且不超过预算上界。
/// 不要求所有预算帧均可闻，不能据此证明没有丢失低幅度尾音。
/// 脚本耗尽后默认继续供数，因此终态失效会暴露多余上游请求。
/// @return 失败断言数量。
int testProviderFinalBoundary()
{
    constexpr ice::AudioDataFormat FORMAT{
        .channels   = 2U,
        .samplerate = 48000U,
    };
    constexpr std::size_t BLOCK_FRAMES = 128U;
    constexpr std::size_t FINAL_FRAMES = 37U;
    // 最后一段刻意短于 128 × 0.75 的通常输入预算，要求 provider 缩短拉取。
    // 结束时按 roundedOutputFrames 对累计输出预算四舍五入，37 / 0.75 得到 49
    // 帧。 本组数值向下取整也恰为 49，不能用此用例单独验证四舍五入规则。
    // 多出的设备块填充不能成为可闻输出。
    constexpr std::size_t EXPECTED_OUTPUT_FRAMES = 49U;

    auto           source = std::make_shared<SegmentSignalNode>();
    BoundaryScript boundaries;
    boundaries.add(FINAL_FRAMES, ice::TimeStretcher::InputBoundary::Final);

    ice::TimeStretcher stretcher;
    stretcher.set_inputnode(source);
    stretcher.set_playback_ratio(0.75);
    stretcher.set_pitch_semitones(-3.0);
    stretcher.set_input_boundary_provider(&boundaries, &readInputBoundary);
    const bool       prepared = stretcher.prepare(FORMAT, BLOCK_FRAMES);
    ice::AudioBuffer output(FORMAT, BLOCK_FRAMES);

    std::size_t audibleFrames = 0U;
    // 跨输出块累计，避免算法延迟使只观察首块的测试误判无声或遗漏尾部填充。
    beginHeapTracking();
    for ( std::size_t block = 0U;
          block < 256U && !stretcher.is_final_input_drained();
          ++block ) {
        stretcher.process(output);
        audibleFrames += countAudibleFrames(output);
    }
    const std::size_t callsWhenDrained = source->callCount();
    // 终态后额外处理一次，检查稳定性，而非只观察首次变为 drained 的瞬间。
    stretcher.process(output);
    endHeapTracking();

    int failures = 0;
    failures += expectTrue(prepared, "provider final state prepares");
    failures += expectTrue(stretcher.is_final_input_drained(),
                           "provider final reaches drained state");
    failures += expectTrue(source->callCount() == 1U,
                           "provider final pulls upstream exactly once");
    failures += expectTrue(source->requestedFrames(0U) == FINAL_FRAMES,
                           "provider final pulls exact remaining frames");
    failures += expectTrue(source->processedFrames() == FINAL_FRAMES,
                           "provider final never over-reads source");
    // 请求次数、单次大小与累计长度交叉核对；输出长度仍用另一条可闻帧断言约束。
    failures += expectTrue(
        audibleFrames > 0U && audibleFrames <= EXPECTED_OUTPUT_FRAMES,
        "provider final trims realtime block padding");
    failures += expectTrue(source->callCount() == callsWhenDrained,
                           "drained provider final remains terminal");
    failures +=
        expectNoHeapAllocation("provider final callbacks do not allocate");
    failures += expectTrue(g_deallocationCount == 0U,
                           "provider final callbacks destroy no C++ objects");
    return failures;
}

/// @brief 验证旁路会忽略 discontinuity 重置但仍严格遵守 Final。
/// @details 默认倍率与音高形成旁路，两段样本及末尾静音按首声道逐帧检查。
/// 忽略算法重置不等于忽略输入分段，上游请求仍必须停在每个边界。
/// 不覆盖旁路与变速状态互相切换时已有尾音的处理。
/// @return 失败断言数量。
int testBypassBoundaryAndFinal()
{
    constexpr ice::AudioDataFormat FORMAT{
        .channels   = 2U,
        .samplerate = 48000U,
    };
    constexpr std::size_t BLOCK_FRAMES = 64U;
    constexpr std::size_t FIRST_FRAMES = 31U;
    constexpr std::size_t FINAL_FRAMES = 17U;

    auto source = std::make_shared<SegmentSignalNode>();
    source->setValues(0.6F, -0.6F);
    BoundaryScript boundaries;
    boundaries.add(FIRST_FRAMES,
                   ice::TimeStretcher::InputBoundary::Discontinuity);
    boundaries.add(FINAL_FRAMES, ice::TimeStretcher::InputBoundary::Final);

    ice::TimeStretcher stretcher;
    stretcher.set_inputnode(source);
    stretcher.set_input_boundary_provider(&boundaries, &readInputBoundary);
    const bool       prepared = stretcher.prepare(FORMAT, BLOCK_FRAMES);
    ice::AudioBuffer output(FORMAT, BLOCK_FRAMES);

    beginHeapTracking();
    stretcher.process(output);
    const std::size_t callsAfterFinal = source->callCount();
    // 样本表借用必须在下一次 process 覆写输出前消费，不保存跨块的内容视图。
    const float* const* firstBlockSamples = output.raw_ptrs();
    bool                firstBlockLayout  = firstBlockSamples != nullptr;
    // 固定窗口容纳 31 + 17 帧有效内容，余下 16 帧专门验证 Final 后静音填充。
    // 同一块依次保留正段、负段和剩余静音，检查边界两侧的次序与 Final 后填充。
    for ( std::size_t frame = 0U; frame < BLOCK_FRAMES && firstBlockLayout;
          ++frame ) {
        const float expected =
            frame < FIRST_FRAMES
                ? 0.6F
                : (frame < FIRST_FRAMES + FINAL_FRAMES ? -0.6F : 0.0F);
        firstBlockLayout &=
            std::abs(firstBlockSamples[0][frame] - expected) < 0.000001F;
    }
    // 布局检查结果按值保存后再复用输出，第二块应保持终态且完全静音。
    stretcher.process(output);
    endHeapTracking();

    const bool terminalBlockSilent = isSilent(output);

    int failures = 0;
    failures += expectTrue(prepared, "bypass boundary state prepares");
    failures += expectTrue(callsAfterFinal == 2U,
                           "bypass crosses discontinuity before final");
    failures += expectTrue(source->requestedFrames(0U) == FIRST_FRAMES,
                           "bypass first segment stops at discontinuity");
    failures += expectTrue(source->requestedFrames(1U) == FINAL_FRAMES,
                           "bypass final segment has exact size");
    failures +=
        expectTrue(source->processedFrames() == FIRST_FRAMES + FINAL_FRAMES,
                   "bypass final never over-reads source");
    failures += expectTrue(firstBlockLayout,
                           "bypass preserves both boundary sides in order");
    failures += expectTrue(stretcher.is_final_input_drained(),
                           "bypass final is immediately drained");
    // 该标志实际在第二次 process 后读取；不能仅靠此处断言证明首块已经立即排空。
    failures += expectTrue(source->callCount() == callsAfterFinal,
                           "bypass terminal callback does not pull upstream");
    failures += expectTrue(terminalBlockSilent,
                           "bypass terminal callback outputs silence");
    failures +=
        expectNoHeapAllocation("bypass boundary callbacks do not allocate");
    failures +=
        expectNoHeapDeallocation("bypass boundary callbacks do not free");
    return failures;
}

/// @brief 验证控制线程持续发布状态时退役回收与音频回调可安全并发。
/// @details 依赖操作系统调度形成竞争，没有强制每次发布均与音频读取重叠。
/// 回调次数大于零只证明工作线程曾执行，不证明全部交错情况或无数据竞争。
/// 仅汇总音频线程的分配及 C++ 释放，不汇总该线程 C free 或控制侧堆操作。
/// @warning join
/// 只在测试收尾阻塞，等待音频线程观察停止标志并退出，不用于业务热路径。
/// @return 失败断言数量。
int testConcurrentStatePublication()
{
    constexpr ice::AudioDataFormat FORMAT{
        .channels   = 2U,
        .samplerate = 48000U,
    };
    constexpr std::size_t BLOCK_FRAMES = 128U;
    constexpr std::size_t STATE_COUNT  = 12U;

    auto               source = std::make_shared<SignalNode>();
    ice::TimeStretcher stretcher;
    stretcher.set_inputnode(source);
    stretcher.set_playback_ratio(0.75);
    const bool prepared = stretcher.prepare(FORMAT, BLOCK_FRAMES);

    /// @brief 控制线程发布的退出请求。
    /// @warning 音频线程每块 acquire 读取，控制线程 release 写入；不打断当前
    /// process。
    std::atomic_bool stop{ false };
    /// @brief 已完成的音频回调数。
    /// @warning 音频侧每块 relaxed 累加，只做观测；主线程在 join 后检查。
    std::atomic<std::size_t> callbackCount{ 0U };
    /// @brief 音频线程退出时发布的 C++ 分配总数。
    /// @warning 工作线程仅在收尾 release 写入，主线程 join 后 acquire
    /// 读取，不在每块刷新。
    std::atomic<std::size_t> allocationCount{ 0U };
    /// @brief 音频线程退出时发布的已捕获 C 堆分配调用数。
    /// @warning 与其他总数分别发布，join 前不能将各字段当作已完成的一致结果。
    std::atomic<std::size_t> mallocCount{ 0U };
    /// @brief 音频线程退出时发布的 C++ 释放总数。
    /// @warning 工作线程收尾写、主线程 join
    /// 后读，不代表线程退出时全部对象已析构。
    std::atomic<std::size_t> deallocationCount{ 0U };
    std::thread              audioThread([&]() {
        // 音频循环不等待控制线程确认每个状态，测试保留真实调度下的发布覆盖机会。
        ice::AudioBuffer output(FORMAT, BLOCK_FRAMES);
        // 缓冲构造不计入回调成本；统计必须在音频线程开启，不能沿用主线程的
        // TLS。
        beginHeapTracking();
        while ( !stop.load(std::memory_order_acquire) ) {
            stretcher.process(output);
            callbackCount.fetch_add(1U, std::memory_order_relaxed);
        }
        endHeapTracking();
        // TLS 不可由主线程直接读出，退出前发布快照；随后 join 也保证线程完成。
        // output 的析构发生在关闭统计之后，不应被解释为回调内释放。
        allocationCount.store(g_allocationCount, std::memory_order_release);
        mallocCount.store(g_mallocCount, std::memory_order_release);
        deallocationCount.store(g_deallocationCount, std::memory_order_release);
    });

    for ( std::size_t state = 0U; state < STATE_COUNT; ++state ) {
        // 交替参数避免重复发布完全相同状态；不插入固定延时来人为等待某次接管。
        const double ratio = state % 2U == 0U ? 0.6 : 1.4;
        const double pitch = state % 3U == 0U ? -5.0 : 4.0;
        stretcher.set_playback_ratio(ratio);
        stretcher.set_pitch_semitones(pitch);
        stretcher.set_quality(state % 2U == 0U
                                  ? ice::TimeStretchQuality::Balanced
                                  : ice::TimeStretchQuality::Finer);
    }
    stop.store(true, std::memory_order_release);
    // 引用捕获的对象必须保留到线程退出，之后才能回收退役状态或离开作用域。
    audioThread.join();
    // 停止标志只让循环退出，不强迫最后一个待发布状态被消费；本用例不检查最终参数。
    stretcher.collect_retired_states();

    int failures = 0;
    failures += expectTrue(prepared, "concurrent state prepares");
    failures += expectTrue(callbackCount.load(std::memory_order_acquire) > 0U,
                           "audio callback ran during state publication");
    // 没有启动握手，若调度直到 stop
    // 发布才运行工作线程，此断言会失败而不是静默跳过。
    failures +=
        expectTrue(allocationCount.load(std::memory_order_acquire) == 0U,
                   "concurrent callback performs no C++ allocation");
    failures += expectTrue(mallocCount.load(std::memory_order_acquire) == 0U,
                           "concurrent callback performs no C allocation");
    failures +=
        expectTrue(deallocationCount.load(std::memory_order_acquire) == 0U,
                   "concurrent callback destroys no C++ objects");
    return failures;
}

}  // namespace

#if defined(ICE_TEST_WRAP_MALLOC)
// 当前构建脚本仅在非 Apple 的 UNIX GNU/Clang 目标启用；Windows Clang64 不启用。
// 因而该配置下的 C 计数为零不提供 C 堆观测证据，不能只根据编译器名称推断覆盖。
// 必须与链接器的 --wrap 选项配套；未启用该宏时，C
// 堆计数不会获得这些入口的事件。
// 包装只覆盖链接器可重定向的调用，不能假定共享库内部或其他分配 API 也被捕获。
/// @brief 链接器提供的未包装 malloc 入口，避免统计转发再次递归进入包装器。
extern "C" void* __real_malloc(std::size_t size);
/// @brief 未包装 calloc 入口，保留运行库的乘法溢出与零初始化处理。
extern "C" void* __real_calloc(std::size_t count, std::size_t size);
/// @brief 未包装 realloc 入口，保留运行库对失败时原指针的所有权约定。
extern "C" void* __real_realloc(void* memory, std::size_t size);
/// @brief 未包装 free 入口，负责实际释放而不重复统计。
extern "C" void __real_free(void* memory);

/// @brief 统计测试线程内的 malloc 调用并转发给真实分配器。
/// @param size 请求字节数，包含零字节请求。
/// @return 真实分配器结果，不把失败改写为成功。
/// @warning 可从被测音频热路径触发；统计自身不得分配、打印或加锁。
extern "C" void* __wrap_malloc(std::size_t size)
{
    // 统计调用尝试而非成功分配，即使真实分配器返回空也应暴露实时堆访问。
    if ( g_trackAllocations ) ++g_mallocCount;
    return __real_malloc(size);
}

/// @brief 统计测试线程内的 calloc 调用并转发给真实分配器。
/// @param count 元素数，包装层不自行计算总字节数。
/// @param size 每个元素的字节数。
/// @return 真实分配器的零初始化存储或空指针。
/// @warning 音频路径触发时只累加线程局部标量，不增加额外同步。
extern "C" void* __wrap_calloc(std::size_t count, std::size_t size)
{
    // 无论元素数多少只记一次入口调用，不能将计数解释为分配了多少个元素。
    if ( g_trackAllocations ) ++g_mallocCount;
    return __real_calloc(count, size);
}

/// @brief 统计测试线程内的 realloc 调用并转发给真实分配器。
/// @param memory 原存储，可为空。
/// @param size 新字节数，原样交由运行库处理。
/// @return 真实 realloc 结果，包装层不更改指针所有权。
/// @warning 可从音频路径触发，不得为观测实际搬迁而维护动态指针表。
extern "C" void* __wrap_realloc(void* memory, std::size_t size)
{
    // 包装层不试图释放旧指针；失败时原指针如何保留由真正的 realloc 决定。
    if ( g_trackAllocations ) {
        // 保守记录潜在分配与释放；原地扩容或失败也计数，不代表真实释放次数。
        ++g_mallocCount;
        if ( memory ) ++g_freeCount;
    }
    return __real_realloc(memory, size);
}

/// @brief 统计测试线程内的 free 调用并转发给真实分配器。
/// @param memory 待释放存储，可为空。
/// @warning 可从音频路径触发，只读取 TLS 开关并累加计数，不进行日志输出。
extern "C" void __wrap_free(void* memory)
{
    // 空指针释放不产生实际回收，因此 C 释放计数忽略它，但仍保持原调用语义。
    if ( g_trackAllocations && memory ) ++g_freeCount;
    __real_free(memory);
}
#endif

/// @brief 测试可执行文件的普通单对象分配计数入口。
/// @param size 请求字节数，直接转交 malloc。
/// @return 分配成功的地址；失败时终止进程，不使用异常。
/// @details
/// 仅替换普通分配入口，未在此实现过对齐分配入口，不能声称覆盖所有分配。
/// 零字节请求未规范化为最小非零大小，沿用 malloc(0) 的运行库结果。
/// 不调用 new_handler；内存不足时不会执行应用层重试或回收策略。
/// @warning 可由被测热路径调用；统计不得再分配或打印，否则可能递归进入本入口。
void* operator new(std::size_t size)
{
    if ( g_trackAllocations ) ++g_allocationCount;
    // 启用 C 包装时同一操作可能同时增加 new 与 malloc
    // 计数，二者不能相加当作对象数。
    if ( void* memory = std::malloc(size) ) return memory;
    // 失败直接中止，后续断言不会执行；本测试没有模拟可恢复的内存不足行为。
    std::abort();
}

/// @brief 测试可执行文件的普通数组分配计数入口。
/// @param size 编译器请求的总字节数，可能包含数组管理开销。
/// @return malloc 结果；失败直接终止进程。
/// @details 计数单位是分配调用，不是数组元素个数；不解析数组布局。
/// @warning 被测热路径触发时只更新 TLS，再转发运行库分配。
void* operator new[](std::size_t size)
{
    // 独立转发而非调用单对象 new，避免单次数组请求增加两次 C++ 计数。
    if ( g_trackAllocations ) ++g_allocationCount;
    if ( void* memory = std::malloc(size) ) return memory;
    std::abort();
}

/// @brief 记录普通单对象释放调用并转发运行库。
/// @param memory 原分配地址。
/// @details 计数不区分指针是否为空，与 C free 包装的空指针计数口径不同。
/// @warning 音频回调内进入此入口即可能触发回收；统计自身不得递归分配。
void operator delete(void* memory) noexcept
{
    // 不核对该地址是否曾在统计窗口内分配，释放历史对象同样会被记录。
    if ( g_trackAllocations ) ++g_deallocationCount;
    std::free(memory);
}

/// @brief 记录普通数组释放调用，不按元素析构次数计数。
/// @param memory 数组分配入口对应的存储地址。
/// @warning 音频路径触发时只累加 TLS 并转发 free，不检查对象内容。
void operator delete[](void* memory) noexcept
{
    // 元素析构由调用者先完成；这里不把元素析构次数计入存储释放计数。
    if ( g_trackAllocations ) ++g_deallocationCount;
    std::free(memory);
}

/// @brief 捕获编译器选择的带大小单对象释放入口。
/// @param memory 待释放存储地址。
/// @details 大小参数不参与释放，存储由 malloc 提供，因此统一交给 free。
/// 不转调本文件另一 delete 重载，避免一次释放被 C++ 计数重复记录。
/// @warning 音频路径触发时仅累加 TLS，不为核对大小维护额外分配表。
void operator delete(void* memory, std::size_t) noexcept
{
    // 由编译器选择普通或带大小入口，测试结果不依赖两种入口分别被调用的比例。
    if ( g_trackAllocations ) ++g_deallocationCount;
    std::free(memory);
}

/// @brief 捕获编译器选择的带大小数组释放入口。
/// @param memory 待释放数组存储地址。
/// @details 忽略大小并直接转发 free，普通和带大小重载各计一次实际入口调用。
/// @warning 音频路径触发时不进行日志输出或对象遍历。
void operator delete[](void* memory, std::size_t) noexcept
{
    // 未验证大小与原分配一致，计数工具不是堆越界或错误释放检测器。
    if ( g_trackAllocations ) ++g_deallocationCount;
    std::free(memory);
}

/// @brief 串行执行实时处理回归用例并汇总退出状态。
/// @return 全部断言通过返回零，否则返回一，供 CTest 判断成功与失败。
/// @details 失败数不会作为退出码直接返回，防止多个失败在进程退出码中截断为零。
/// 普通断言失败继续执行后续用例；分配失败的 abort 或其他崩溃不在此恢复。
/// 每个用例自行构造和销毁节点，不共享待发布状态或边界脚本，避免测试间隐式状态依赖。
/// 全局分配入口与线程局部计数仍由整个进程共享，新增用例必须关闭自身统计窗口。
int main()
{
    int failures = 0;
    failures += testRealtimeStateAndReset();
    // 邮箱 final 与 provider Final
    // 由不同用例触发，不可将其中一条入口覆盖替代另一条。
    failures += testFinalDrain();
    failures += testCapacityGuard();
    failures += testPauseFreezesPipeline();
    failures += testFractionalInputFrameRemainder();
    failures += testExtremePreparedStates();
    failures += testPendingStateWaitsForFinalDrain();
    failures += testRealtimeStartCompensation();
    // 算法边界采用幅度/预算检查，旁路边界可逐样本检查，两者的证据强度不同。
    failures += testStretchedLoopBoundary();
    failures += testProviderFinalBoundary();
    failures += testBypassBoundaryAndFinal();
    // 并发用例自行 join 收尾；其 TLS 统计不会继承此前主线程的计数窗口。
    failures += testConcurrentStatePublication();
    return failures == 0 ? 0 : 1;
}
