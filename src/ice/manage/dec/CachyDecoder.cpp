#include <algorithm>
#include <cstring>
#include <ice/manage/dec/CachyDecoder.hpp>

namespace ice {
// 构造即创建线程全部解码数据放入缓存
CachyDecoder::CachyDecoder(std::string_view file) : IDecoder(file) {
    // TODO(xiang 2025-07-11): 解码全部pcm数据
}

size_t CachyDecoder::decode(float** buffer, uint16_t num_channels,
                            size_t start_frame, size_t frame_count) {
    // 写入数据到buffer中,并返回此次实际写入的帧数(可能到末尾实际写入帧数不=frame_count)
    // 如果内部没有数据,或请求的起始点已超出范围，则直接返回
    if (pcm.empty() || start_frame >= num_internal_frames()) {
        return 0;
    }
    // 确定要拷贝的帧数:取请求帧数和剩余帧数中的较小值
    const size_t frames_available = num_internal_frames() - start_frame;
    const size_t frames_to_copy = std::min(frame_count, frames_available);
    if (frames_to_copy == 0) {
        return 0;
    }
    // 确定要拷贝的声道数:取源和目标声道数中的较小值
    const uint16_t channels_to_copy =
        std::min((uint16_t)pcm.size(), num_channels);
    // 逐声道进行内存拷贝
    for (uint16_t ch = 0; ch < channels_to_copy; ++ch) {
        // 源指针:指向内部PCM数据的正确起始位置
        const float* src = pcm[ch].data() + start_frame;
        // 目标指针:从传入的指针数组中获取
        float* dest = buffer[ch];
        // 直接块拷贝内存
        memcpy(dest, src, frames_to_copy * sizeof(float));
    }
    // 返回实际处理的帧数
    return frames_to_copy;
}
}  // namespace ice
