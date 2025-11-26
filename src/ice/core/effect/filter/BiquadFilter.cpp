#include <cmath>
#include <ice/core/effect/filter/BiquadFilter.hpp>
#include <numbers>

namespace ice
{

// 从Audio EQ Cookbook来的标准公式
void BiquadFilter::set_peaking(double sample_rate, double center_freq_hz,
                               double q, double gain_db)
{
    const double A      = std::pow(10.0, gain_db / 40.0);
    const double w0     = 2.0 * std::numbers::pi * center_freq_hz / sample_rate;
    const double cos_w0 = std::cos(w0);
    const double sin_w0 = std::sin(w0);
    const double alpha  = sin_w0 / (2.0 * q);

    const double b0_temp = 1.0 + alpha * A;
    const double b1_temp = -2.0 * cos_w0;
    const double b2_temp = 1.0 - alpha * A;
    // 这是分母
    const double a0_temp = 1.0 + alpha / A;
    const double a1_temp = -2.0 * cos_w0;
    const double a2_temp = 1.0 - alpha / A;

    // 归一化系数
    b0 = b0_temp / a0_temp;
    b1 = b1_temp / a0_temp;
    b2 = b2_temp / a0_temp;
    a1 = a1_temp / a0_temp;
    a2 = a2_temp / a0_temp;
}

void BiquadFilter::process(float* data, size_t num_frames)
{
    for ( size_t i = 0; i < num_frames; ++i )
        {
            double in = data[i];

            // 应用滤波器公式
            double out = b0 * in + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;

            // 更新状态
            x2 = x1;
            x1 = in;
            y2 = y1;
            y1 = out;

            // 写回输出
            data[i] = static_cast<float>(out);
        }
}
}  // namespace ice
