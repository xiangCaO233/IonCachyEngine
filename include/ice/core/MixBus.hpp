#ifndef ICE_MIXBUS_HPP
#define ICE_MIXBUS_HPP

#include <memory>
#include <mutex>
#include <set>

#include "ice/core/IAudioNode.hpp"
#include "ice/manage/AudioBuffer.hpp"

namespace ice
{
// 混音总线(可选)
class MixBus : public IAudioNode
{
public:
    // 构造MixBus
    MixBus() = default;
    // 析构MixBus
    ~MixBus() override = default;

    void process(AudioBuffer& buffer) override;

    void add_source(std::shared_ptr<IAudioNode> src);

    void remove_source(std::shared_ptr<IAudioNode> src);

    inline void clear()
    {
        std::lock_guard<std::mutex> lock(sources_mutex);
        sources.clear();
    }

private:
    // 来源表
    std::set<std::shared_ptr<ice::IAudioNode>> sources;

    // 保护 sources 的互斥锁
    std::mutex sources_mutex;

    // 缓存的缓冲区
    AudioBuffer temp_buffer;
};
}  // namespace ice

#endif  // ICE_MIXBUS_HPP
