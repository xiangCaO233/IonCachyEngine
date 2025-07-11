#ifndef ICE_SOURCENODE_HPP
#define ICE_SOURCENODE_HPP

#include <atomic>
#include <cstddef>
#include <memory>

#include "ice/core/IAudioNode.hpp"
#include "ice/manage/AudioTrack.hpp"

namespace ice {
class SourceNode : public IAudioNode {
   public:
    // 构造SourceNode
    SourceNode(std::shared_ptr<AudioTrack> t);
    // 析构SourceNode
    ~SourceNode() override = default;

    // 只管从播放位置读取请求的数据量并填充缓冲区
    void process(AudioBuffer& buffer, uint32_t frame_count) override;

   protected:
    // 轨道指针
    std::shared_ptr<AudioTrack> track;

    // 播放位置
    std::atomic<size_t> playback_pos;

    // 音源音量
    std::atomic<float> volume;

    // 音源是否循环(到结尾是否自动置零播放位置)
    std::atomic<bool> is_looping;
};
}  // namespace ice

#endif  // ICE_SOURCENODE_HPP
