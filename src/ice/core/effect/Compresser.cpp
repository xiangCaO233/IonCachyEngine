#include <cmath>
#include <ice/core/effect/Compresser.hpp>

namespace ice
{

// 应用压缩器效果
void Compressor::apply_effect(AudioBuffer& output, const AudioBuffer& input)
{
    // 确保输入和输出缓冲区是兼容的
    if ( output.num_frames() != input.num_frames() ||
         output.num_channels() != input.num_channels() )
        {
            // 直接返回
            return;
        }

    // 将输入拷贝到输出
    for ( uint16_t ch = 0; ch < input.num_channels(); ++ch )
        {
            std::memcpy(output.raw_ptrs()[ch],
                        input.raw_ptrs()[ch],
                        input.num_frames() * sizeof(float));
        }
    const auto&    fmt          = output.afmt;
    const size_t   num_frames   = output.num_frames();
    const uint16_t num_channels = fmt.channels;

    // 冷路径:更新内部系数
    if ( needs_update || fmt.samplerate != sample_rate )
        {
            sample_rate = fmt.samplerate;

            // 根据毫秒计算平滑系数
            // 公式: coeff = exp(-1.0 / (time_ms * sample_rate * 0.001))
            attack_coeff =
                std::exp(-1.0 / (attack_ms.load() * sample_rate * 0.001));
            release_coeff =
                std::exp(-1.0 / (release_ms.load() * sample_rate * 0.001));

            // 将补充增益从dB转换为线性倍率
            makeup_gain_linear = std::pow(10.0, makeup_gain_db.load() / 20.0);

            // 调整包络状态数组的大小以匹配声道数
            envelope_db.resize(num_channels, 0.0);

            needs_update = false;
        }

    // 加载原子变量到局部变量,避免在热循环中反复加载
    const float thresh_db = threshold_db.load();
    const float ratio_val = ratio.load();

    // 热路径:逐样本处理
    for ( uint16_t ch = 0; ch < num_channels; ++ch )
        {
            float* channel_data = output.raw_ptrs()[ch];

            for ( size_t i = 0; i < num_frames; ++i )
                {
                    float in_sample = channel_data[i];

                    // 电平检测:将当前样本的振幅转换为dB
                    // 使用绝对值来检测电平，并加上一个极小值防止log(0)
                    double sample_db =
                        20.0 * std::log10(std::fabs(in_sample) + 1e-9);

                    // 包络跟随:平滑地更新“感知到的音量” (envelope_db_)
                    // 如果新来的样本更响，就用更快的 attack_coeff_ 去逼近它
                    // 如果新来的样本更轻，就用更慢的 release_coeff_ 去回落
                    if ( sample_db > envelope_db[ch] )
                        {
                            envelope_db[ch] =
                                sample_db +
                                attack_coeff * (envelope_db[ch] - sample_db);
                        }
                    else
                        {
                            envelope_db[ch] =
                                sample_db +
                                release_coeff * (envelope_db[ch] - sample_db);
                        }

                    // 增益计算:根据当前感知到的音量，计算需要施加多大的衰减
                    double gain_reduction_db = 0.0;
                    if ( envelope_db[ch] > thresh_db )
                        {
                            // 音量超过了阈值
                            // 计算超出的部分
                            double overshoot_db = envelope_db[ch] - thresh_db;
                            // 根据压缩比，计算需要压掉多少dB
                            // e.g., 4:1 ratio means 1/4 passes through, 3/4 is
                            // reduced.
                            gain_reduction_db =
                                overshoot_db * (1.0 - 1.0 / ratio_val);
                        }

                    // 增益应用:将计算出的衰减和补充增益，一起应用到原始样本上
                    // 总增益 = 补充增益 - 计算出的衰减
                    double total_gain_db =
                        makeup_gain_db.load() - gain_reduction_db;
                    // 将总增益从dB转换为线性倍率
                    double total_gain_linear =
                        std::pow(10.0, total_gain_db / 20.0);

                    // 应用最终的增益
                    channel_data[i] =
                        in_sample * static_cast<float>(total_gain_linear);
                }
        }
}

}  // namespace ice
