#include <cmath>
#include <complex>
#include <ice/core/effect/filter/BiquadFilter.hpp>
#include <numbers>

namespace ice
{

/// @brief 根据 Audio EQ Cookbook 峰值均衡公式生成归一化双二阶系数。
/// @pre 采样率和 Q 为正，频率位于有效采样带内，所有参数有限。
/// @warning 控制侧系数计算包含三角函数，不能与本实例的 process 并发。
/// 改系数不清除历史采样；平滑更新或重新建立状态由上层决定。
void BiquadFilter::set_peaking(double sample_rate, double center_freq_hz,
                               double q, double gain_db)
{
    const double A      = std::pow(10.0, gain_db / 40.0);
    const double w0     = 2.0 * std::numbers::pi * center_freq_hz / sample_rate;
    const double cos_w0 = std::cos(w0);
    const double sin_w0 = std::sin(w0);
    const double alpha  = sin_w0 / (2.0 * q);
    // Q 控制带宽，系数计算不裁剪频率或增益，参数有效性由控制侧保证。

    const double b0_temp = 1.0 + alpha * A;
    const double b1_temp = -2.0 * cos_w0;
    const double b2_temp = 1.0 - alpha * A;
    // 增益用幅度平方根 A 分配到分子和分母，正负 dB 对应峰值提升与衰减。
    const double a0_temp = 1.0 + alpha / A;
    const double a1_temp = -2.0 * cos_w0;
    const double a2_temp = 1.0 - alpha / A;

    // 除以 a0 使递推分母首项为 1，process 才无需逐样本重复归一化。
    b0 = b0_temp / a0_temp;
    b1 = b1_temp / a0_temp;
    b2 = b2_temp / a0_temp;
    a1 = a1_temp / a0_temp;
    a2 = a2_temp / a0_temp;
}

/// @brief 原位处理单声道采样，并保留跨块递推历史。
/// @param data 可读写至少 num_frames 个 float 的连续平面。
/// @param num_frames 本次有效帧数，零帧不改变历史。
/// @warning 每个采样执行常数次算术，无分配、锁或原子操作，禁止加入这些操作。
void BiquadFilter::process(float* data, size_t num_frames)
{
    for ( size_t i = 0; i < num_frames; ++i ) {
        double in = data[i];

        // 直接型递推使用两个输入与输出历史，反馈项按分母符号减去。
        double out = b0 * in + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;

        // 先后移再写当前值，避免覆盖下一步仍要使用的一阶历史。
        x2 = x1;
        x1 = in;
        y2 = y1;
        y1 = out;

        // 内部历史保留 double 精度，仅输出到图的 float 平面时收窄。
        data[i] = static_cast<float>(out);
    }
}

/// @brief 在单位圆上计算当前系数的线性幅度响应，不修改滤波历史。
/// @pre 采样率为正，参数有限，系数在查询期间不被另一个线程修改。
/// @return 幅度绝对值而非 dB，串联滤波器可将该值相乘。
double BiquadFilter::get_magnitude_response(double frequency,
                                            double sample_rate) const
{
    const double w = 2.0 * std::numbers::pi * frequency / sample_rate;

    // H(z) = (b0 + b1*z^-1 + b2*z^-2) / (1 + a1*z^-1 + a2*z^-2)
    // z^-1 = cos(w) - j*sin(w)
    const std::complex<double> z_inv1(std::cos(w), -std::sin(w));
    const std::complex<double> z_inv2 = z_inv1 * z_inv1;
    // 查询只依赖系数，不受之前处理过的音频内容或当前延迟状态影响。

    std::complex<double> num = b0 + b1 * z_inv1 + b2 * z_inv2;
    std::complex<double> den = 1.0 + a1 * z_inv1 + a2 * z_inv2;

    // 极点导致分母趋零时沿用复数运算结果，此查询不钳制显示增益。
    return std::abs(num / den);
}

}  // namespace ice
