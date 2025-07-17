#ifndef ICE_RSTRETCHER_HPP
#define ICE_RSTRETCHER_HPP

#include <rubberband/RubberBandStretcher.h>

#include <atomic>

#include "ice/manage/AudioBuffer.hpp"
#include "ice/manage/AudioFormat.hpp"

namespace ice {
class RStretcher {
   public:
    // 构造RStretcher
    explicit RStretcher(const AudioDataFormat& format) {
        rubberband_stretcher = std::make_unique<
            RubberBand::RubberBandStretcher>(
            format.samplerate, format.channels,
            RubberBand::RubberBandStretcher::OptionProcessRealTime |  // 实时
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
            stretcher_ratio.load(), palt_ratio.load());
    };

    // 析构RStretcher
    ~RStretcher() = default;

    inline void set_stretch_ratio(double ratio) {
        stretcher_ratio.store(ratio);
        rubberband_stretcher->setTimeRatio(ratio);
    }

    inline void set_pitch_ratio(double pitch_ratio) {
        palt_ratio.store(pitch_ratio);
        rubberband_stretcher->setPitchScale(pitch_ratio);
    }

    inline double get_stretch_ratio() const { return stretcher_ratio.load(); }

    // 流式处理
    inline void process(AudioBuffer& output, const AudioBuffer& input) {
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
    std::atomic<double> stretcher_ratio{1.};
    std::atomic<double> palt_ratio{1.};
    std::unique_ptr<RubberBand::RubberBandStretcher> rubberband_stretcher;
};
}  // namespace ice
#endif  // ICE_RSTRETCHER_HPP
