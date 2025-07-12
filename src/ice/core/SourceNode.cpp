#include <ice/core/SourceNode.hpp>

namespace ice {
// 构造SourceNode
SourceNode::SourceNode(std::shared_ptr<AudioTrack> t) : track(t) {}

// 只管读取数据填充缓冲区
void SourceNode::process(AudioBuffer& buffer, uint32_t frame_count) {
    track->read(buffer, playback_pos.load(), frame_count);
}

}  // namespace ice
