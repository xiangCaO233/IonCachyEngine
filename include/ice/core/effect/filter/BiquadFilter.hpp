#ifndef ICE_BIQUADFILTER_HPP
#define ICE_BIQUADFILTER_HPP

#include <cstddef>

namespace ice
{
/// @brief 保存单声道系数与两阶输入输出历史的峰值均衡滤波器。
/// 一个实例只能串行处理一个声道；各声道和各级滤波器需要独立历史。
class BiquadFilter
{
public:
    /// @brief 初始化为单位传递函数，历史清零，未设置参数时直接透传。
    BiquadFilter() = default;

    /// @brief 更新峰值均衡系数而不清除历史，采样率、Q 必须为正且参数有限。
    /// @warning 控制侧计算接口，不得与本实例处理或响应查询并发执行。
    void set_peaking(double sample_rate, double center_freq_hz, double q,
                     double gain_db);

    /// @brief 原位处理单声道块，data 可写至少 num_frames 个采样。
    /// @warning 逐样本热路径，禁止分配、锁、原子和日志，历史值由单线程独占。
    void process(float* data, size_t num_frames);

    /**
     * @brief 获取滤波器在指定频率处的幅频响应 (增益)
     * @param frequency 目标频率 (Hz)
     * @param sample_rate 采样率
     * @return 幅度增益 (线性值)
     */
    double get_magnitude_response(double frequency, double sample_rate) const;

private:
    /// @brief 分子当前采样系数，默认 1 提供单位增益。
    double b0{ 1.0 };
    /// @brief 分子一阶输入历史系数。
    double b1{ 0.0 };
    /// @brief 分子二阶输入历史系数。
    double b2{ 0.0 };
    /// @brief 归一化分母一阶系数，在递推中以负号参与反馈。
    double a1{ 0.0 };
    /// @brief 归一化分母二阶系数，a0 已归约为 1 不单独存储。
    double a2{ 0.0 };

    /// @brief 前一个输入采样，块间保留以维持滤波连续性。
    double x1{ 0.0 };
    /// @brief 前两个输入采样中的较早值。
    double x2{ 0.0 };
    /// @brief 前一个 double 精度输出，尚未收窄为 float。
    double y1{ 0.0 };
    /// @brief 前两个输出采样中的较早值。
    double y2{ 0.0 };
};
}  // namespace ice

#endif  // ICE_BIQUADFILTER_HPP
