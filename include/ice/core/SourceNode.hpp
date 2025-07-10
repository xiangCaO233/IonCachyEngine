#ifndef ICE_SOURCENODE_HPP
#define ICE_SOURCENODE_HPP

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

    // 只管读取数据填充缓冲区
    void process(AudioBuffer& buffer, uint32_t frame_count) override;

   protected:
    std::shared_ptr<AudioTrack> track;
};
}  // namespace ice

#endif  // ICE_SOURCENODE_HPP
