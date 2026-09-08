#include "ice/core/effect/GraphicEqualizer.hpp"

#include "ice/config/config.hpp"
#include "ice/core/effect/filter/BiquadFilter.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

namespace ice
{
/// @brief 控制侧构建后发布的滤波链，系数只读，递推历史由单一音频线程修改。
/// 各声道独立存储历史，禁止将同一状态交给多个并发音频回调处理。
struct GraphicEqualizer::PreparedFilterState {
    /// @brief 状态适用的固定音频格式。
    AudioDataFormat format{};

    /// @brief 每个声道独立的滤波器链及其历史值。
    std::vector<std::vector<BiquadFilter>> filterChains;
};

/// @brief 按固定频率列表创建单位增益频段，并预备默认格式状态。
/// @pre 各中心频率有限且适用于后续采样率，频段次序由调用方确定。
/// @warning 构造和 prepare 包含分配，仅用于接入音频图之前。
GraphicEqualizer::GraphicEqualizer(const std::vector<double>& centerFrequencies)
{
    static_assert(std::atomic<PreparedFilterState*>::is_always_lock_free,
                  "GraphicEqualizer 的音频线程状态指针必须为无锁原子");

    // 一次建立固定频段数，运行中的参数更新只替换增益或 Q，不改变拓扑。
    m_bands.reserve(centerFrequencies.size());
    for ( const double frequency : centerFrequencies ) {
        m_bands.push_back(EQBandOptions{ .center_freq_hz = frequency });
    }

    prepare(ICEConfig::internal_format,
            // 默认配置为零块长时仍预备至少一帧，避免基类输入容量为空。
            std::max<std::size_t>(ICEConfig::default_buffer_size, 1U));
}

/// @brief 在音频线程停止使用后回收当前状态及所有退役链。
/// @warning 析构不等待 hazard 清除，调用方必须先停止音频处理。
GraphicEqualizer::~GraphicEqualizer() = default;

/// @brief 准备基类输入容量，并按新格式重建全部滤波链。
/// @warning 可分配与释放，且基类缓冲不受控制锁保护，只能脱离运行音频图调用。
void GraphicEqualizer::prepare(const AudioDataFormat& format,
                               std::size_t            maxFrames)
{
    IEffectNode::prepare(format, maxFrames);
    // 先准备图输入存储，再发布对应采样率的系数，避免混用新旧格式。

    std::lock_guard<std::mutex> lock(m_controlMutex);
    m_preparedFormat = format;
    publish_filter_state_locked();
}

/// @brief 将正幅度倍率转为 dB，并发布重建后的完整滤波链。
/// @pre 输入有限；小于等于阈值的倍率忽略，不能用零倍率表达静音。
/// @warning 控制侧低频更新，包含锁、分配与旧状态回收。
void GraphicEqualizer::set_band_gain_ratio(std::size_t bandIndex, float ratio)
{
    if ( ratio <= 0.0001F ) return;
    // 阈值检查先于锁与对数计算，拒绝值不触发状态分配或发布。

    std::lock_guard<std::mutex> lock(m_controlMutex);
    if ( bandIndex >= m_bands.size() ) return;

    // 幅度比使用 20log10，而非功率比的 10log10，与 Biquad 峰值增益约定一致。
    m_bands[bandIndex].gain_db = 20.0 * std::log10(static_cast<double>(ratio));
    publish_filter_state_locked();
}

/// @brief 替换单段 dB 参数，越界索引忽略，不扩展频段数组。
/// @pre db 有限，调用方负责产品层允许的增益范围。
/// @warning 控制侧更新重建全部声道和频段，不能逐采样或在音频回调调用。
void GraphicEqualizer::set_band_gain_db(std::size_t bandIndex, float db)
{
    std::lock_guard<std::mutex> lock(m_controlMutex);
    if ( bandIndex >= m_bands.size() ) return;

    m_bands[bandIndex].gain_db = db;
    // 即使数值未变也重建状态，接口不提供去重或连续调参消抖。
    publish_filter_state_locked();
}

/// @brief 更新指定频段的正 Q，非正值忽略。
/// @pre q 有限；有效参数更新会重置新滤波链的递推历史。
/// @warning 控制侧加锁重建，不在音频线程现场更新系数。
void GraphicEqualizer::set_band_q_factor(std::size_t bandIndex, float q)
{
    if ( q <= 0.0F ) return;
    // 非正 Q 会使滤波系数无效；有限性仍由上游配置校验负责。

    std::lock_guard<std::mutex> lock(m_controlMutex);
    if ( bandIndex >= m_bands.size() ) return;

    m_bands[bandIndex].q_factor = q;
    publish_filter_state_locked();
}

/// @brief 在控制锁下查询固定频段数。
/// @warning 获取锁，只供 UI 或控制侧查询，不能用于音频回调。
std::size_t GraphicEqualizer::get_band_count() const
{
    std::lock_guard<std::mutex> lock(m_controlMutex);
    return m_bands.size();
}

/// @brief 返回指定频率，越界时以零作为查询失败的占位值。
/// @warning 控制锁查询，不可放进逐样本处理链。
double GraphicEqualizer::get_band_frequency(std::size_t bandIndex) const
{
    std::lock_guard<std::mutex> lock(m_controlMutex);
    return bandIndex < m_bands.size() ? m_bands[bandIndex].center_freq_hz : 0.0;
}

/// @brief 返回控制侧已设增益；越界返回 0dB，不代表索引有效。
/// @warning 读取需要控制锁，不用于音频热路径。
double GraphicEqualizer::get_band_gain_db(std::size_t bandIndex) const
{
    std::lock_guard<std::mutex> lock(m_controlMutex);
    return bandIndex < m_bands.size() ? m_bands[bandIndex].gain_db : 0.0;
}

/// @brief 返回控制侧 Q，越界时返回兼容默认值 1。
/// @warning 读取需要控制锁，不用于音频热路径。
double GraphicEqualizer::get_band_q_factor(std::size_t bandIndex) const
{
    std::lock_guard<std::mutex> lock(m_controlMutex);
    return bandIndex < m_bands.size() ? m_bands[bandIndex].q_factor : 1.0;
}

/// @brief 用当前控制参数计算各峰值滤波器串联后的线性幅度。
/// @warning UI 或控制侧查询，持锁并计算三角函数，不读取音频递推历史。
double GraphicEqualizer::get_total_magnitude_response(double frequency) const
{
    std::lock_guard<std::mutex> lock(m_controlMutex);
    if ( m_preparedFormat.samplerate == 0U ) return 1.0;
    // 无有效采样率时按旁路显示；空频段列表也自然返回单位增益。

    double totalMagnitude = 1.0;
    for ( const EQBandOptions& band : m_bands ) {
        BiquadFilter responseFilter;
        // 临时滤波器只查询系数响应，不共享或扰动正在播放的状态。
        responseFilter.set_peaking(
            static_cast<double>(m_preparedFormat.samplerate),
            band.center_freq_hz,
            band.q_factor,
            band.gain_db);
        totalMagnitude *= responseFilter.get_magnitude_response(
            frequency, static_cast<double>(m_preparedFormat.samplerate));
    }
    return totalMagnitude;
}

/// @brief 在控制侧显式触发退役状态回收，不要求同时发布新参数。
/// @warning 获取控制锁并可能释放嵌套滤波数组，不得由音频线程调用。
void GraphicEqualizer::reclaim_retired_filter_states()
{
    std::lock_guard<std::mutex> lock(m_controlMutex);
    reclaim_retired_filter_states_locked();
}

/// @brief 返回尚在退役列表中的状态数，不包含当前发布状态。
/// 数量为零只能说明本次查询没有退役所有权，不能作为线程停止证明。
/// @warning 低频诊断查询持控制锁，不能用作音频处理同步条件。
std::size_t GraphicEqualizer::retired_filter_state_count() const
{
    std::lock_guard<std::mutex> lock(m_controlMutex);
    return m_retiredStates.size();
}

/// @brief 先复制输入作为旁路结果，再使用受保护的状态原位串联滤波。
/// @warning 每个音频块调用，音频侧发布 hazard，控制侧读取它以延迟释放。
/// 只借用状态指针；禁止分配、释放、控制锁与共享所有权复制。
/// @pre 输入输出存储独立且已准备，单个实例仅有一个音频读取者。
void GraphicEqualizer::apply_effect(AudioBuffer&       output,
                                    const AudioBuffer& input)
{
    if ( output.afmt != input.afmt ||
         output.num_frames() != input.num_frames() ) {
        // 格式和块长不一致时失败静音，不能在音频回调通过 resize 修复。
        output.clear();
        return;
    }

    float**             outputSamples = output.raw_ptrs();
    const float* const* inputSamples  = input.raw_ptrs();
    if ( !outputSamples || !inputSamples ) {
        // 无可访问平面时不进入逐声道处理，保留无分配失败路径。
        output.clear();
        return;
    }

    for ( std::uint16_t channel = 0U; channel < input.num_channels();
          ++channel ) {
        // 全部有效帧先复制，后面的任意旁路返回都不暴露上次输出的残留采样。
        std::memcpy(outputSamples[channel],
                    inputSamples[channel],
                    input.num_frames() * sizeof(float));
    }

    PreparedFilterState* const state = acquire_filter_state();
    // 无状态或状态暂不匹配时保留已复制的输入，输出为旁路而非旧缓存。
    if ( !state ) return;
    // 验证完成后所有滤波迭代使用同一个状态，块内不会混合新旧频段参数。

    if ( state->format != output.afmt ||
         state->filterChains.size() != output.num_channels() ) {
        // 即使未应用任何滤波也必须释放 hazard，控制侧才能回收该旧状态。
        release_filter_state();
        return;
    }

    for ( std::uint16_t channel = 0U; channel < output.num_channels();
          ++channel ) {
        float* channelSamples = outputSamples[channel];
        for ( BiquadFilter& filter : state->filterChains[channel] ) {
            // 同一声道按固定频段顺序串联，各 filter 历史只被本音频线程修改。
            filter.process(channelSamples, output.num_frames());
        }
    }
    release_filter_state();
    // 最后一个 filter 返回后才解除保护，控制侧此后可安全销毁整条旧链。
}

/// @brief 在控制锁下完整构造新状态，再发布稳定地址并尝试回收旧状态。
/// @pre 调用方持 m_controlMutex，所有参数及格式在本次构造期间保持一致。
/// @warning 低频控制侧分配，不得把构造或退役容器访问移到音频线程。
void GraphicEqualizer::publish_filter_state_locked()
{
    auto nextState    = std::make_unique<PreparedFilterState>();
    nextState->format = m_preparedFormat;
    nextState->filterChains.resize(m_preparedFormat.channels);
    // 每次重建获得零历史滤波器，参数切换不复制旧延迟值，也不做渐变过渡。

    for ( auto& chain : nextState->filterChains ) {
        // 为每个声道建立相同系数但独立历史，避免左右声道互相污染反馈状态。
        chain.resize(m_bands.size());
        for ( std::size_t bandIndex = 0U; bandIndex < m_bands.size();
              ++bandIndex ) {
            const EQBandOptions& band = m_bands[bandIndex];
            // 按本次预备采样率计算，频率参数本身保持 Hz 单位，不原地归一化。
            chain[bandIndex].set_peaking(
                static_cast<double>(m_preparedFormat.samplerate),
                band.center_freq_hz,
                band.q_factor,
                band.gain_db);
        }
    }

    PreparedFilterState* const nextAddress = nextState.get();
    // 先把旧所有权移入退役链，再替换公开地址，旧读取者仍由 hazard 保活。
    if ( m_activeStateOwner ) {
        // 移动 unique_ptr 只转移控制侧所有权，不移动音频线程借用的对象地址。
        m_retiredStates.push_back(std::move(m_activeStateOwner));
    }
    m_activeStateOwner = std::move(nextState);
    // 对象完整建立后才发布裸地址，读者永远不参与对象分配或引用计数。
    m_activeState.store(nextAddress, std::memory_order_seq_cst);
    // 发布后才回收，读者若读到旧地址必须通过二次验证才能开始解引用。
    reclaim_retired_filter_states_locked();
}

/// @brief 保留单读取者正在保护的状态，释放其余退役所有权。
/// @pre 持有控制锁，所有发布者串行；当前状态不在退役列表内。
/// @warning 控制侧读取音频侧 hazard；顺序一致语义参与跨原子验证协议。
void GraphicEqualizer::reclaim_retired_filter_states_locked()
{
    PreparedFilterState* const protectedState =
        m_hazardState.load(std::memory_order_seq_cst);
    // 若读者先观察旧 active 再发布保护，其二次校验会看到替换并重新获取。
    // hazard 只可能保护一个状态；保护结束后还需下一次控制侧回收触发释放。
    std::erase_if(
        m_retiredStates,
        [protectedState](const std::unique_ptr<PreparedFilterState>& state) {
            // 比较地址即可判断是否保留，不读取可能正被音频更新的滤波历史。
            return state.get() != protectedState;
        });
}

/// @brief 在读取状态前发布 hazard，并验证控制侧没有替换公开地址。
/// @warning 音频每块一次；读取控制侧 active、写入音频侧 hazard，再复读 active。
/// 顺序一致的跨原子顺序用于阻止提前回收，不能孤立弱化某一次访问。
/// 当前循环在持续发布竞争下可能重试，不提供等待次数上界。
GraphicEqualizer::PreparedFilterState*
GraphicEqualizer::acquire_filter_state() noexcept
{
    PreparedFilterState* state{ nullptr };
    do {
        // 验证成功前不解引用候选地址，即使控制侧已回收旧候选也不会访问其内容。
        state = m_activeState.load(std::memory_order_seq_cst);
        m_hazardState.store(state, std::memory_order_seq_cst);
        // 只有后续复读仍一致才可解引用；不一致时先重新发布候选的保护。
    } while ( state != m_activeState.load(std::memory_order_seq_cst) );
    // 空 active 也通过同一协议，此时 hazard 为空且调用方直接旁路。
    // 返回地址有效期截止 release，调用方不能缓存到下一个音频块使用。
    return state;
}

/// @brief 清除本音频块的保护，只通知控制侧可回收，不在此释放资源。
/// @warning 音频每块一次，写入 hazard 空值供控制侧读取，禁止添加回收或锁。
void GraphicEqualizer::release_filter_state() noexcept
{
    m_hazardState.store(nullptr, std::memory_order_seq_cst);
}

}  // namespace ice
