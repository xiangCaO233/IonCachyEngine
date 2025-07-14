#ifndef ICE_SOURCENODE_HPP
#define ICE_SOURCENODE_HPP

#include <atomic>
#include <memory>

#include "ice/core/IAudioNode.hpp"

namespace ice {
class AudioTrack;
class Resampler;
class SourceNode : public IAudioNode {
   public:
    // 构造SourceNode
    explicit SourceNode(std::shared_ptr<AudioTrack> track,
                        const AudioDataFormat& engin_format);
    // 析构SourceNode
    ~SourceNode() override = default;

    // 只管从播放位置读取请求的数据量并填充缓冲区
    void process(AudioBuffer& buffer) override;

    // 设置是否循环播放
    inline void setloop(bool flag) { is_looping.store(flag); }

   private:
    // 重采样实现
    std::unique_ptr<Resampler> resampleimpl{nullptr};

    // 轨道指针
    std::shared_ptr<AudioTrack> track;

    // 播放位置
    std::atomic<size_t> playback_pos{0};

    // 音源音量
    std::atomic<float> volume{0.4f};

    // 音源是否循环(到结尾是否自动置零播放位置)
    std::atomic<bool> is_looping{false};

    // 应用音量到缓冲区
    void apply_volume(AudioBuffer& buffer) const;
};
}  // namespace ice

#endif  // ICE_SOURCENODE_HPP
