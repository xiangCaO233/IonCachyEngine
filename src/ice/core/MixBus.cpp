#include <ice/core/IAudioNode.hpp>
#include <ice/core/MixBus.hpp>

namespace ice {
void MixBus::process(AudioBuffer& buffer) {
    // 先清空buffer
    buffer.clear();
    // 拉取全部来源数据
    for (const auto& source : sources) {
        temp_buffer.clear();
        temp_buffer.resize(buffer.afmt, buffer.num_frames());
        // 拉取输入源的数据到缓冲
        source->process(temp_buffer);

        // 混合到上游请求的buffer中
        buffer += temp_buffer;
    }
}

void MixBus::add_source(std::shared_ptr<IAudioNode> src) {
    sources.insert(src);
}

void MixBus::remove_source(std::shared_ptr<IAudioNode> src) {
    sources.erase(src);
}
}  // namespace ice
