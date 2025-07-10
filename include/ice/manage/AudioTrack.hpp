#ifndef ICE_AUDIOTRACK_HPP
#define ICE_AUDIOTRACK_HPP

#include <atomic>
#include <cstdint>
#include <memory>
#include <string_view>

#include "ice/manage/AudioBuffer.hpp"
#include "ice/manage/codec/IDecoder.hpp"

namespace ice {
class AudioTrack {
   public:
    // 构造AudioTrack
    explicit AudioTrack(std::string_view file);

    // 析构AudioTrack
    virtual ~AudioTrack() = default;

    std::shared_ptr<IDecoder> specific_decoder;

    // 对应文件
    std::string_view file_view;

    // 音频文件的格式
    AudioFormat format;

    // 当前播放位置以目标采样率的帧为单位
    std::atomic<size_t> pos{0};
    // 音量
    std::atomic<float> track_vol{1.0f};

    // 读取pcm
    inline void pcm(AudioBuffer& buffer, uint32_t frames) {}

    inline void seek(size_t p) { pos.store(p); }
    inline void set_volume(float vol) { track_vol.store(vol); }
};

}  // namespace ice

#endif  // ICE_AUDIOTRACK_HPP
