#include <ice/core/SourceNode.hpp>
#include <ice/execptions/load_error.hpp>
#include <ice/manage/AudioTrack.hpp>

extern "C" {
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>
}

#define AVCALL_CHECK(func)                          \
    if (int ret = func < 0) {                       \
        char errbuf[AV_ERROR_MAX_STRING_SIZE];      \
        av_strerror(ret, errbuf, sizeof(errbuf));   \
        throw ice::load_error(std::string(errbuf)); \
    }

namespace ice {
class Resampler {
   public:
    Resampler(const AudioDataFormat& source_format,
              const AudioDataFormat& target_format) {
        // 初始化重采样器 SwrContext
        AVChannelLayout srcch_layout;
        av_channel_layout_default(&srcch_layout, source_format.channels);
        AVChannelLayout tgtch_layout;
        av_channel_layout_default(&tgtch_layout, target_format.channels);
        AVCALL_CHECK(swr_alloc_set_opts2(
            &swr_ctx,
            &tgtch_layout,       // 目标声道布局
            AV_SAMPLE_FMT_FLTP,  // 目标样本格式固定为planner-float(32位浮点数，平面)
            target_format.samplerate,  // 目标采样率
            &srcch_layout,             // 源声道布局
            AV_SAMPLE_FMT_FLTP,        // 源样本格式
            source_format.samplerate,  // 源采样率
            0,                         // 日志偏移
            nullptr                    // 日志上下文
            ))

        if (!swr_ctx) {
            throw ice::load_error("swr_alloc_set_opts failed.");
        }

        // 初始化重采样器上下文
        AVCALL_CHECK(swr_init(swr_ctx))
    }
    ~Resampler() {
        if (swr_ctx) {
            swr_close(swr_ctx);
        }
    }
    void resmaple(AudioBuffer& buffer) {
        AudioBuffer conversion_buffer;
        // 实现格式转换传入的缓冲区为buffer的afmt-已经配置好了
        conversion_buffer.resize(buffer.afmt, buffer.num_frames());
        // 执行转换：从临时源转换到最终的目标buffer。
        swr_convert(swr_ctx, (uint8_t**)conversion_buffer.raw_ptrs(),
                    buffer.num_frames(), (const uint8_t**)buffer.raw_ptrs(),
                    buffer.num_frames());
        // 直接移动缓冲区
        buffer = std::move(conversion_buffer);
    }

   private:
    SwrContext* swr_ctx{nullptr};
};

// 构造SourceNode
SourceNode::SourceNode(std::shared_ptr<AudioTrack> track,
                       const AudioDataFormat& engin_format)
    : track(track) {
    if (engin_format != track->get_media_info().format) {
        // 格式不同-创建一个重采器
        resampleimpl = std::make_unique<Resampler>(
            track->get_media_info().format, engin_format);
    }
}

// 析构SourceNode
SourceNode::~SourceNode() = default;

// 只管读取数据填充缓冲区
void SourceNode::process(AudioBuffer& buffer) {
    if (!is_playing.load()) return;

    auto gained_frames = track->read(buffer, playback_pos, buffer.num_frames());
    if (gained_frames < buffer.num_frames()) {
        // 未完全获取到预期的帧数-在此处补0
        // 非常路径-提醒编译器优化
        [[unlikely]] buffer.clear_from(gained_frames);
    }

    // 更新播放位置
    playback_pos += gained_frames;
    [[unlikely]] if (playback_pos >= track->get_media_info().frame_count) {
        // 启用循环则在此恢复播放指针到0
        if (is_looping.load()) {
            playback_pos = 0;
        } else {
            playback_pos.store(track->get_media_info().frame_count);
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
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                double_seconds(static_cast<double>(playback_pos.load()) /
                               format().samplerate)));
    }

    if (resampleimpl) {
        // 若需要格式转换则在此执行转换
        resampleimpl->resmaple(buffer);
    }
    if (volume != 1.f) {
        apply_volume(buffer);
    }
}
// 应用音量到缓冲区
void SourceNode::apply_volume(AudioBuffer& buffer) const {
    const uint16_t num_channels = buffer.afmt.channels;
    const size_t num_frames = buffer.num_frames();
    // 如果无事可做，立刻退出。这是一个关键的微优化。
    if (num_frames == 0) {
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
