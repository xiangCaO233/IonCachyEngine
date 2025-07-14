#include <ice/core/MixBus.hpp>

namespace ice {
void MixBus::process(AudioBuffer& buffer) {
    // 先清空buffer
    buffer.clear();
    // 拉取全部来源数据
    for (const auto& source : sources) {
        // 拉取输入源的数据到缓冲
        source->process(temp_buffer);

        // 混合到上游请求的buffer中
        buffer += temp_buffer;
    }
}
}  // namespace ice
