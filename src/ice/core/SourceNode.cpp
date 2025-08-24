#include <ice/core/SourceNode.hpp>
#include <ice/execptions/load_error.hpp>
#include <ice/manage/AudioTrack.hpp>

namespace ice {

// 构造SourceNode
SourceNode::SourceNode(std::shared_ptr<AudioTrack> track) : track(track) {}

// 析构SourceNode
SourceNode::~SourceNode() = default;

// 只管读取数据填充缓冲区
void SourceNode::process(AudioBuffer& buffer) {
    if (!is_playing.load()) return;

    buffer.resize(track->get_media_info().format, buffer.num_frames());

    auto gained_frames = track->read(buffer, playback_pos, buffer.num_frames());

    if (gained_frames < buffer.num_frames()) {
        // 未完全获取到预期的帧数-在此处补0
        // 非常路径-提醒编译器优化
        [[unlikely]] buffer.clear_from(gained_frames);
    }

    // 更新播放位置
    playback_pos += gained_frames;
    [[unlikely]] if (playback_pos >= track->num_frames()) {
        // 启用循环则在此恢复播放指针到0
        if (is_looping.load()) {
            playback_pos = 0;
        } else {
            playback_pos.store(track->num_frames());
        }

        // 通知回调播放完成一遍
        for (const auto& callback : callbacks) {
            callback->play_done(is_looping.load());
        }

        [[unlikely]] if (gained_frames == 0)
            return;
    }

    // 通知回调
    for (const auto& callback : callbacks) {
        callback->frameplaypos_updated(playback_pos.load());
        // 转换播放位置为纳秒
        using double_seconds = std::chrono::duration<double>;
        callback->timeplaypos_updated(
            std::chrono::duration_cast<std::chrono::nanoseconds>(double_seconds(
                static_cast<double>(playback_pos.load()) /
                double(ice::ICEConfig::internal_format.samplerate))));
    }

    if (volume != 1.f) {
        apply_volume(buffer);
    }
}
// 应用音量到缓冲区
void SourceNode::apply_volume(AudioBuffer& buffer) const {
    const uint16_t num_channels = buffer.afmt.channels;
    const size_t num_frames = buffer.num_frames();
    // 如果无事可做,立刻退出
    if (num_frames == 0 ||
        std::abs(volume - 1.f) <= std::numeric_limits<float>::epsilon()) {
        return;
    }
    // 热循环-SIMD优化的主要候选者
    for (uint16_t ch = 0; ch < num_channels; ++ch) {
        // 获取指向该声道数据起始位置的指针
        float* channel_data = buffer.raw_ptrs()[ch];
        // 在紧凑连续循环中处理采样点。
        for (size_t i = 0; i < num_frames; ++i) {
            channel_data[i] *= volume;
        }
    }
}

}  // namespace ice
