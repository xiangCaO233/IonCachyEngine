#ifndef ICE_GRAPHICEQUALIZER_HPP
#define ICE_GRAPHICEQUALIZER_HPP

#include "ice/core/effect/IEffectNode.hpp"
#include "ice/manage/AudioFormat.hpp"

#include <atomic>
#include <cstddef>
#include <memory>
#include <mutex>
#include <numbers>
#include <vector>

namespace ice
{
/// @brief 图形均衡器的单个频段参数。
struct EQBandOptions {
    /// @brief 中心频率，单位 Hz。
    double center_freq_hz{ 0.0 };

    /// @brief 品质因数。
    double q_factor{ std::numbers::sqrt2 };

    /// @brief 增益，单位 dB。
    double gain_db{ 0.0 };
};

/// @brief 通过控制侧预备状态实现实时安全参数热更新的图形均衡器。
class GraphicEqualizer : public IEffectNode
{
public:
    /// @brief 使用固定中心频率列表构造均衡器。
    /// @param centerFrequencies 各频段中心频率，单位 Hz。
    explicit GraphicEqualizer(const std::vector<double>& centerFrequencies);

    /// @brief 析构均衡器。
    /// @warning 销毁前必须停止调用 process 的音频线程。
    ~GraphicEqualizer() override;

    /// @brief 预备输入缓冲区及当前参数对应的滤波状态。
    /// @param format 音频回调使用的固定格式。
    /// @param maxFrames 单个 block 允许的最大帧数。
    /// @warning 该函数会分配内存，只能在节点尚未接入运行中音频图时调用。
    void prepare(const AudioDataFormat& format, std::size_t maxFrames);

    /// @brief 按线性倍率设置指定频段增益。
    /// @param bandIndex 频段索引。
    /// @param ratio 正线性倍率。
    void set_band_gain_ratio(std::size_t bandIndex, float ratio);

    /// @brief 按 dB 设置指定频段增益。
    /// @param bandIndex 频段索引。
    /// @param db 增益，单位 dB。
    void set_band_gain_db(std::size_t bandIndex, float db);

    /// @brief 设置指定频段的品质因数。
    /// @param bandIndex 频段索引。
    /// @param q 正品质因数。
    void set_band_q_factor(std::size_t bandIndex, float q);

    /// @brief 获取频段数量。
    /// @return 固定频段数量。
    [[nodiscard]] std::size_t get_band_count() const;

    /// @brief 获取指定频段的中心频率。
    /// @param bandIndex 频段索引。
    /// @return 中心频率，单位 Hz；索引无效时返回零。
    [[nodiscard]] double get_band_frequency(std::size_t bandIndex) const;

    /// @brief 获取指定频段的增益。
    /// @param bandIndex 频段索引。
    /// @return 增益，单位 dB；索引无效时返回零。
    [[nodiscard]] double get_band_gain_db(std::size_t bandIndex) const;

    /// @brief 获取指定频段的品质因数。
    /// @param bandIndex 频段索引。
    /// @return 品质因数；索引无效时返回 1。
    [[nodiscard]] double get_band_q_factor(std::size_t bandIndex) const;

    /// @brief 获取当前控制参数在指定频率处的整体幅频响应。
    /// @param frequency 目标频率，单位 Hz。
    /// @return 线性幅度增益。
    /// @warning UI 或控制线程路径：会获取控制锁，不得在音频回调调用。
    [[nodiscard]] double get_total_magnitude_response(double frequency) const;

    /// @brief 回收已经越过音频读取临界区的历史滤波状态。
    /// @warning 该函数可能释放内存并获取控制锁，不得在音频回调调用。
    void reclaim_retired_filter_states();

    /// @brief 获取尚未回收的历史滤波状态数量。
    /// @return 历史状态数量。
    /// @warning 该函数会获取控制锁，不得在音频回调调用。
    [[nodiscard]] std::size_t retired_filter_state_count() const;

protected:
    /// @brief 对一个已拉取的 block 应用当前稳定滤波状态。
    /// @param output 输出缓冲区。
    /// @param input 输入缓冲区。
    /// @warning 音频回调热路径：只在 block 边界取得一次状态快照，不得分配、
    /// 释放、获取锁或遍历控制侧容器。
    void apply_effect(AudioBuffer& output, const AudioBuffer& input) override;

private:
    /// @brief 控制线程完整构造、音频线程独占修改历史值的滤波状态。
    struct PreparedFilterState;

    /// @brief 根据当前控制参数构造并发布新滤波状态。
    /// @warning 调用方必须持有 m_controlMutex。
    void publish_filter_state_locked();

    /// @brief 回收不再被音频线程 hazard 指针保护的历史状态。
    /// @warning 调用方必须持有 m_controlMutex。
    void reclaim_retired_filter_states_locked();

    /// @brief 在 block 起点取得并保护当前滤波状态。
    /// @return 当前稳定状态。
    /// @warning 音频回调热路径：使用单读取者 hazard 协议，不得改为
    /// shared_ptr 或锁。
    PreparedFilterState* acquire_filter_state() noexcept;

    /// @brief 结束当前 block 的滤波状态读取临界区。
    /// @warning 音频回调热路径：只清除 hazard 指针，不执行回收。
    void release_filter_state() noexcept;

    /// @brief 控制线程维护的频段参数。
    std::vector<EQBandOptions> m_bands;

    /// @brief 当前滤波状态对应的音频格式。
    AudioDataFormat m_preparedFormat{};

    /// @brief 当前发布状态的控制侧独占所有权。
    std::unique_ptr<PreparedFilterState> m_activeStateOwner;

    /// @brief 等待音频线程越过读取临界区的历史状态。
    std::vector<std::unique_ptr<PreparedFilterState>> m_retiredStates;

    /// @brief 当前发布给音频线程的滤波状态地址。
    /// @warning 控制线程写、单一音频线程读；顺序一致语义与 hazard
    /// 指针共同阻止状态提前回收。
    std::atomic<PreparedFilterState*> m_activeState{ nullptr };

    /// @brief 音频线程当前正在使用的滤波状态地址。
    /// @warning 单一音频线程写、控制线程读；顺序一致语义用于安全回收。
    std::atomic<PreparedFilterState*> m_hazardState{ nullptr };

    /// @brief 串行化参数读写、状态构造和历史状态回收。
    mutable std::mutex m_controlMutex;
};

}  // namespace ice

#endif  // ICE_GRAPHICEQUALIZER_HPP
