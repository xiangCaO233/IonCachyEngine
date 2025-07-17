#include <rubberband/RubberBandStretcher.h>

#include <atomic>
#include <cmath>
#include <ice/core/effect/TimeStretcher.hpp>
#include <memory>

#include "ice/manage/AudioFormat.hpp"

namespace ice {
class StretcherImp {
   public:
    explicit StretcherImp(AudioDataFormat& format) {
        rubberband_stretcher =
            std::make_unique<RubberBand::RubberBandStretcher>(
                format.samplerate, format.channels,
                RubberBand::RubberBandStretcher::
                        OptionEngineFiner |  // R3引擎,时间精确
                    RubberBand::RubberBandStretcher::
                        OptionChannelsTogether |  // 所有声道一起分析
                    RubberBand::RubberBandStretcher::
                        OptionWindowStandard |  // 标准窗口
                    RubberBand::RubberBandStretcher::
                        OptionPitchHighQuality  // 高质量
                    | RubberBand::RubberBandStretcher::
                          OptionThreadingAuto,  // 自动选择线程
                1.0, 1.0);
    }
    ~StretcherImp() = default;

    inline void set_stretch_ratio(double ratio) {
        rubberband_stretcher->setTimeRatio(ratio);
        stretcher_ratio.store(ratio);
    }

    inline double get_stretch_ratio() const { return stretcher_ratio.load(); }

    // 流式拉伸
    inline void stretch(AudioBuffer& output, const AudioBuffer& input) {
        const size_t input_frames = input.num_frames();
        const size_t output_frames_capacity = output.num_frames();
        const uint16_t num_channels = input.afmt.channels;

        // 将输入数据喂给 RubberBand
        // process() 的最后一个参数是 false，表示后面可能还有数据
        rubberband_stretcher->process(input.raw_ptrs(), input_frames, false);

        // 从 RubberBand 中取出所有可用的数据，直到填满输出缓冲区
        size_t total_retrieved = 0;

        while (total_retrieved < output_frames_capacity) {
            int avail = rubberband_stretcher->available();
            if (avail <= 0) {
                // 没有更多可用的样本了，退出
                break;
            }

            size_t frames_to_retrieve = std::min(
                (size_t)avail, output_frames_capacity - total_retrieved);

            // 准备一个临时的指针数组，指向输出缓冲区的正确偏移位置
            std::vector<float*> output_ptrs(num_channels);
            for (uint16_t ch = 0; ch < num_channels; ++ch) {
                output_ptrs[ch] = output.raw_ptrs()[ch] + total_retrieved;
            }

            auto actual = rubberband_stretcher->retrieve(output_ptrs.data(),
                                                         frames_to_retrieve);
            if (actual <= 0) break;

            total_retrieved += actual;
        }

        // 如果取出的数据不足以填满输出缓冲区，将剩余部分清零
        if (total_retrieved < output_frames_capacity) {
            output.clear_from(total_retrieved);
        }
    }

   private:
    std::atomic<double> stretcher_ratio;
    std::unique_ptr<RubberBand::RubberBandStretcher> rubberband_stretcher;
};

TimeStretcher::TimeStretcher() = default;
TimeStretcher::~TimeStretcher() = default;

void TimeStretcher::process(AudioBuffer& buffer) {
    auto input_node = get_inputnode();
    if (!input_node) {
        buffer.clear();
        return;
    }

    // 加载用户设定的期望倍率
    const auto desired_ratio = desired_playback_ratio.load();

    // 直通优化
    if (std::fabs(desired_ratio - 1.0f) < 0.001f) {
        // 报告实际倍率
        actual_playback_ratio.store(1.0f);
        input_node->process(buffer);
        return;
    }

    // 拉伸处理
    const size_t output_frames_needed = buffer.num_frames();
    if (output_frames_needed == 0) return;

    // 计算为了生成 N 帧输出,需要消耗多少 M 帧输入
    // 输入帧数 = 输出帧数 * 期望播放速度
    // 需要一个整数的输入帧数,使用四舍五入
    const auto input_frames_to_pull = static_cast<size_t>(
        std::round(static_cast<double>(output_frames_needed) * desired_ratio));

    // 如果计算出的输入为0,则无法处理,输出静音
    if (input_frames_to_pull == 0) {
        // 报告期望值，虽然没干活
        actual_playback_ratio.store(desired_ratio);
        buffer.clear();
        return;
    }

    // 根据整数的输入/输出帧数，计算出本次处理实际的、精确的拉伸倍率
    // 真正的拉伸倍率 (Time-Stretch Ratio) = 输出 / 输入
    actual_stretch_ratio.store(static_cast<double>(output_frames_needed) /
                               static_cast<double>(input_frames_to_pull));

    // 真正的播放速度倍率 (Playback Ratio) = 输入 / 输出 (拉伸倍率的倒数)
    const auto actual_ratio = static_cast<double>(input_frames_to_pull) /
                              static_cast<double>(output_frames_needed);

    // 存储实际生效的倍率,供外部查询
    actual_playback_ratio.store(actual_ratio);

    // 准备输入缓冲区
    AudioBuffer& input_buf = get_inputbuffer();
    input_buf.resize(buffer.afmt, input_frames_to_pull);

    // 拉取数据
    input_node->process(input_buf);

    // 调用 apply_effect
    apply_effect(buffer, input_buf);
}

// 应用效果实现
void TimeStretcher::apply_effect(AudioBuffer& output,
                                 const AudioBuffer& input) {
    if (!stretchimpl) {
        // 需要拉伸且不存在拉伸器-在此初始化
        stretchimpl = std::make_unique<StretcherImp>(output.afmt);
    }
    // 更新拉伸倍率
    if (stretchimpl->get_stretch_ratio() != actual_stretch_ratio) {
        stretchimpl->set_stretch_ratio(actual_stretch_ratio.load());
    }

    // 执行拉伸
    stretchimpl->stretch(output, input);
}

}  // namespace ice
