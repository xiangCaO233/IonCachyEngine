#ifndef ICE_AUDIOFORMAT_HPP
#define ICE_AUDIOFORMAT_HPP

#include <cstdint>

namespace ice
{
/// @brief 平面浮点音频的声道数与采样率契约，不记录样本所有权。
/// 帧表示每个声道各一个采样，不能把帧数与总采样数混用。
struct AudioDataFormat {
    /// @brief 平面数量，默认立体声；处理器应拒绝零声道格式。
    uint16_t channels{ 2 };
    /// @brief 每秒采样帧数，默认 48 kHz，用于帧与时间换算。
    uint32_t samplerate{ 48000 };
    /// @brief 声道数与采样率均相同时格式相等，不进行任何重采样。
    /// 缓冲内容不参与比较，比较成功也不意味着缓冲有足够容量。
    bool operator==(const AudioDataFormat& other) const = default;
};
}  // namespace ice

#endif  // ICE_AUDIOFORMAT_HPP
