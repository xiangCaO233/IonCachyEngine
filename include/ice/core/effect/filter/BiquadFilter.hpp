#ifndef ICE_BIQUADFILTER_HPP
#define ICE_BIQUADFILTER_HPP

#include <cstddef>

namespace ice
{
class BiquadFilter
{
public:
    BiquadFilter() = default;

    // 设置滤波器参数
    void set_peaking(double sample_rate, double center_freq_hz, double q,
                     double gain_db);

    // 对一个音频块进行处理
    void process(float* data, size_t num_frames);

private:
    // 滤波器系数
    double b0{ 1.0 };
    double b1{ 0.0 };
    double b2{ 0.0 };
    double a1{ 0.0 };
    double a2{ 0.0 };

    // 单声道状态变量（每个声道都需要一对）
    // 上两次的输入
    double x1{ 0.0 };
    double x2{ 0.0 };
    // 上两次的输出
    double y1{ 0.0 };
    double y2{ 0.0 };
};
}  // namespace ice

#endif  // ICE_BIQUADFILTER_HPP
