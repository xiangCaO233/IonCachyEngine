#include <cmath>
#include <ice/core/effect/Compresser.hpp>

namespace ice
{

/// @brief 基类构造已经准备输入格式，随后补齐默认声道包络。
Compressor::Compressor()
{
    prepare_effect(get_inputbuffer().afmt);
}

/// @brief 保留仍在使用的声道包络，新增声道延用初始零 dB。
void Compressor::prepare_effect(const AudioDataFormat& format)
{
    // 声道变化与采样率独立；在控制侧调整状态，处理阶段保持尺寸固定。
    envelope_db.resize(format.channels, 0.0);
}

/// @brief 逐声道跟随包络并施加阈值以上的动态衰减。
/// @warning 每音频块调用，只更新预分配的包络状态，禁止扩容和同步操作。
/// 参数在控制侧停止处理时修改，时间常数或采样率变化后只在块开始重算系数。
void Compressor::apply_effect(AudioBuffer& output, const AudioBuffer& input)
{
    // 仅检查帧数与声道数，不校验两块缓冲采样率；正常基类调用应已保证格式一致。
    if ( output.num_frames() != input.num_frames() ||
         output.num_channels() != input.num_channels() ) {
        // 不兼容时保留输出原内容，不主动静音，调用方不可将正常返回视为有效输出。
        return;
    }

    // 后续原位修改输出，输入保持只读；这里依赖两块样本存储不重叠。
    for ( uint16_t ch = 0; ch < input.num_channels(); ++ch ) {
        std::memcpy(output.raw_ptrs()[ch],
                    input.raw_ptrs()[ch],
                    input.num_frames() * sizeof(float));
    }
    const auto&    fmt          = output.afmt;
    const size_t   num_frames   = output.num_frames();
    const uint16_t num_channels = fmt.channels;

    // 系数由时间常数和采样率决定；声道尺寸已在格式准备阶段同步。
    if ( needs_update || fmt.samplerate != sample_rate ) {
        sample_rate = fmt.samplerate;

        // 将毫秒时间常数换成单样本指数衰减；时间为正且采样率有效才能得到稳定系数。
        attack_coeff  = std::exp(-1.0 / (attack_ms * sample_rate * 0.001));
        release_coeff = std::exp(-1.0 / (release_ms * sample_rate * 0.001));

        needs_update = false;
    }

    // 整块使用同一组阈值、压缩系数和补偿增益；逐样本只计算信号相关部分。
    // 比率除法与输入无关，移到块边界仍保持原增益公式及运算精度。
    const double thresh_db       = threshold_db;
    const double reduction_ratio = 1.0 - 1.0 / ratio;
    const double makeup_db       = makeup_gain_db;

    // 热路径:逐样本处理
    for ( uint16_t ch = 0; ch < num_channels; ++ch ) {
        float* channel_data = output.raw_ptrs()[ch];

        for ( size_t i = 0; i < num_frames; ++i ) {
            float in_sample = channel_data[i];

            // 电平检测:将当前样本的振幅转换为dB
            // 使用绝对值来检测电平，并加上一个极小值防止log(0)
            double sample_db = 20.0 * std::log10(std::fabs(in_sample) + 1e-9);

            // 在 dB 域平滑，不是先平滑线性振幅再换算电平。
            // 上升与下降按电平比较选择系数；调用方配置决定哪个时间常数更快。
            if ( sample_db > envelope_db[ch] ) {
                envelope_db[ch] =
                    sample_db + attack_coeff * (envelope_db[ch] - sample_db);
            } else {
                envelope_db[ch] =
                    sample_db + release_coeff * (envelope_db[ch] - sample_db);
            }

            // 增益计算:根据当前感知到的音量，计算需要施加多大的衰减
            double gain_reduction_db = 0.0;
            if ( envelope_db[ch] > thresh_db ) {
                // 仅处理超阈值部分，低于阈值时仍会应用独立补偿增益。
                double overshoot_db = envelope_db[ch] - thresh_db;
                // 根据压缩比，计算需要压掉多少dB
                // 例如 4:1 只保留超阈值部分的四分之一，而非缩小整个输入。
                gain_reduction_db = overshoot_db * reduction_ratio;
            }

            // 增益应用:将计算出的衰减和补充增益，一起应用到原始样本上
            // 总增益 = 补充增益 - 计算出的衰减
            double total_gain_db = makeup_db - gain_reduction_db;
            // 将总增益从dB转换为线性倍率
            double total_gain_linear = std::pow(10.0, total_gain_db / 20.0);

            // 不对最终样本限幅，正补偿或低于一的比率都可能让输出超过单位幅度。
            channel_data[i] = in_sample * static_cast<float>(total_gain_linear);
        }
    }
}

}  // namespace ice
