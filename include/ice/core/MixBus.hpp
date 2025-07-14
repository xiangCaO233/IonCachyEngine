#ifndef ICE_MIXBUS_HPP
#define ICE_MIXBUS_HPP

#include <list>
#include <memory>

#include "ice/core/IAudioNode.hpp"
#include "ice/manage/AudioBuffer.hpp"

namespace ice {
// 混音总线(可选)
class MixBus : public IAudioNode {
   public:
    // 构造MixBus
    MixBus();
    // 析构MixBus
    ~MixBus() override;

    void process(AudioBuffer& buffer) override;

   private:
    // 来源表
    std::list<std::shared_ptr<ice::IAudioNode>> sources;

    // 缓存的缓冲区
    AudioBuffer temp_buffer;
};
}  // namespace ice

#endif  // ICE_MIXBUS_HPP
