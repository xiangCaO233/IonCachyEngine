#include <cmath>
#include <ice/core/effect/Compresser.hpp>

namespace ice
{

/// @brief 逐声道跟随包络并施加阈值以上的动态衰减。
/// @warning 每音频块调用；当前系数更新可能扩容包络，尚非完整实时安全实现。
/// 控制 setter 同时修改非原子脏标记，必须在处理停止时调整参数。
/// @warning 处理读取控制侧原子参数，当前默认顺序一致；补偿值甚至逐样本读取。
/// 本路径仍有可替代的历史原子及扩容，注释不构成实时约束已满足的证明。
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

    // 低频条件分支仍位于音频调用栈，不能因为不常执行就视为控制线程路径。
    // 系数更新会重设声道状态容量；实时预准备需要在独立修复中处理。
    if ( needs_update || fmt.samplerate != sample_rate ) {
        sample_rate = fmt.samplerate;

        // 将毫秒时间常数换成单样本指数衰减；时间为正且采样率有效才能得到稳定系数。
        attack_coeff =
            std::exp(-1.0 / (attack_ms.load() * sample_rate * 0.001));
        release_coeff =
            std::exp(-1.0 / (release_ms.load() * sample_rate * 0.001));

        // 此缓存目前未用于逐样本乘法；实际增益仍在下方重新读取并转换。
        makeup_gain_linear = std::pow(10.0, makeup_gain_db.load() / 20.0);

        // resize 保留已有元素，新声道从零 dB 开始，并非每次更新都重置所有包络。
        // 此条件不包含声道变化，单独改变声道数而不置脏可能使状态数组尺寸不匹配。
        envelope_db.resize(num_channels, 0.0);

        needs_update = false;
    }

    // 阈值和压缩比每块各取一次快照；补偿增益仍由下方逐样本读取。
    // 原子参数本身不能使非原子 needs_update 变为可并发访问。
    const float thresh_db = threshold_db.load();
    const float ratio_val = ratio.load();

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
                gain_reduction_db = overshoot_db * (1.0 - 1.0 / ratio_val);
            }

            // 增益应用:将计算出的衰减和补充增益，一起应用到原始样本上
            // 总增益 = 补充增益 - 计算出的衰减
            double total_gain_db = makeup_gain_db.load() - gain_reduction_db;
            // 将总增益从dB转换为线性倍率
            double total_gain_linear = std::pow(10.0, total_gain_db / 20.0);

            // 不对最终样本限幅，正补偿或低于一的比率都可能让输出超过单位幅度。
            channel_data[i] = in_sample * static_cast<float>(total_gain_linear);
        }
    }
}

}  // namespace ice
