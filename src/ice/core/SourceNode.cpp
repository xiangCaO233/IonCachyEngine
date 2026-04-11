#include <ice/core/SourceNode.hpp>
#include <ice/execptions/load_error.hpp>
#include <ice/manage/AudioTrack.hpp>

namespace ice
{

// 构造SourceNode
SourceNode::SourceNode(std::shared_ptr<AudioTrack> track) : track(track) {}

// 析构SourceNode
SourceNode::~SourceNode() = default;

// 只管读取数据填充缓冲区
void SourceNode::process(AudioBuffer& buffer)
{
    if ( !is_playing.load() ) return;

    size_t gained_this_frame = 0;
    size_t scheduled_start   = scheduled_start_frame.load();
    if ( scheduled_start > 0 && ref_pos_provider ) {
        size_t current_ref = ref_pos_provider();
        if ( current_ref < scheduled_start ) {
            size_t frames_to_wait = scheduled_start - current_ref;
            if ( frames_to_wait >= buffer.num_frames() ) {
                // 还没到时间，且这一整块都需要等待
                buffer.resize(ice::ICEConfig::internal_format,
                              buffer.num_frames());
                buffer.clear();
                return;
            } else {
                // 这一块的前面部分是静音，后面部分开始播放
                size_t silence_frames = frames_to_wait;
                size_t play_frames    = buffer.num_frames() - silence_frames;

                buffer.resize(ice::ICEConfig::internal_format,
                              buffer.num_frames());
                buffer.clear();  // 先全清空

                // 为后半部分读取数据
                AudioBuffer temp;
                temp.resize(ice::ICEConfig::internal_format, play_frames);
                auto gained = track->read(temp, playback_pos, play_frames);
                if ( gained < play_frames ) {
                    temp.clear_from(gained);
                }

                // 将数据拷贝到 buffer 的后半段
                for ( uint16_t ch = 0; ch < buffer.afmt.channels; ++ch ) {
                    std::copy(temp.raw_ptrs()[ch],
                              temp.raw_ptrs()[ch] + play_frames,
                              buffer.raw_ptrs()[ch] + silence_frames);
                }

                playback_pos += gained;
                gained_this_frame = gained;
                scheduled_start_frame.store(0);  // 标记已触发
            }
        } else {
            // 已经过了预定时间
            scheduled_start_frame.store(0);
        }
    }

    if ( scheduled_start_frame.load() == 0 && gained_this_frame == 0 ) {
        buffer.resize(ice::ICEConfig::internal_format, buffer.num_frames());
        auto gained_frames =
            track->read(buffer, playback_pos, buffer.num_frames());
        if ( gained_frames < buffer.num_frames() ) {
            [[unlikely]] buffer.clear_from(gained_frames);
        }
        playback_pos += gained_frames;
        gained_this_frame = gained_frames;
    }

    // 更新播放位置与回调逻辑... (复用原有逻辑)
    [[unlikely]] if ( playback_pos >= track->num_frames() ) {
        // 启用循环则在此恢复播放指针到0
        if ( is_looping.load() ) {
            playback_pos = 0;
        } else {
            playback_pos.store(track->num_frames());
            pause();
        }

        // 通知回调播放完成一遍
        for ( const auto& callback : callbacks ) {
            callback->play_done(is_looping.load());
        }

        [[unlikely]] if ( gained_this_frame == 0 )
            return;
    }

    // 通知回调
    for ( const auto& callback : callbacks ) {
        callback->frameplaypos_updated(playback_pos.load());
        // 转换播放位置为纳秒
        using double_seconds = std::chrono::duration<double>;
        callback->timeplaypos_updated(
            std::chrono::duration_cast<std::chrono::nanoseconds>(double_seconds(
                static_cast<double>(playback_pos.load()) /
                double(ice::ICEConfig::internal_format.samplerate))));
    }

    if ( volume != 1.f ) {
        apply_volume(buffer);
    }
}
// 应用音量到缓冲区
void SourceNode::apply_volume(AudioBuffer& buffer) const
{
    const uint16_t num_channels = buffer.afmt.channels;
    const size_t   num_frames   = buffer.num_frames();
    // 如果无事可做,立刻退出
    if ( num_frames == 0 ||
         std::abs(volume - 1.f) <= std::numeric_limits<float>::epsilon() ) {
        return;
    }
    // 热循环-SIMD优化的主要候选者
    for ( uint16_t ch = 0; ch < num_channels; ++ch ) {
        // 获取指向该声道数据起始位置的指针
        float* channel_data = buffer.raw_ptrs()[ch];
        // 在紧凑连续循环中处理采样点。
        for ( size_t i = 0; i < num_frames; ++i ) {
            channel_data[i] *= volume;
        }
    }
}

}  // namespace ice
