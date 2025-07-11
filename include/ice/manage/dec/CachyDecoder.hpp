#ifndef ICE_CACHYDECODER_HPP
#define ICE_CACHYDECODER_HPP

#include <string_view>
#include <vector>

#include "ice/manage/dec/IDecoder.hpp"

namespace ice {
class CachyDecoder : public IDecoder {
    using ChannelData = std::vector<float>;

   public:
    // 构造CachyDecoder
    explicit CachyDecoder(std::string_view file);

    // 内部缓存的总帧数
    inline size_t num_internal_frames() const {
        return pcm.empty() ? 0 : pcm[0].size();
    }

    // 解码数据到缓冲区的接口
    size_t decode(float** buffer, uint16_t num_channels, size_t start_frame,
                  size_t frame_count) override;

   private:
    std::vector<ChannelData> pcm;
};

}  // namespace ice

#endif  // ICE_CACHYDECODER_HPP
