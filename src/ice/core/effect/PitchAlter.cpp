#include <rubberband/RubberBandStretcher.h>

#include <cmath>
#include <ice/core/effect/PitchAlter.hpp>

#include "ice/manage/AudioFormat.hpp"

namespace ice {
class PAlterImp {
   public:
    explicit PAlterImp(const AudioDataFormat& format) {
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
            1.0, 1.0);
    }

    ~PAlterImp() = default;

    inline void set_pitch_ratio(double pitch_ratio) {
        rubberband_stretcher->setPitchScale(pitch_ratio);
    }

    inline void shift(AudioBuffer& output, const AudioBuffer& input) {
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
    std::atomic<double> alt_ratio;
    std::unique_ptr<RubberBand::RubberBandStretcher> rubberband_stretcher;
};

PitchAlter::PitchAlter() = default;
PitchAlter::~PitchAlter() = default;

// 应用变调器
void PitchAlter::apply_effect(AudioBuffer& output, const AudioBuffer& input) {
    // 惰性初始化
    if (!palterimpl) {
        palterimpl = std::make_unique<PAlterImp>(input.afmt);
    }

    // 设置 RubberBand 的参数
    // 音高倍率 = 2 ^ (半音数 / 12)
    const double pitch_ratio = std::pow(2.0, pitch_semitones.load() / 12.0);
    palterimpl->set_pitch_ratio(pitch_ratio);

    // 调用内部的、封装了RubberBand实时处理循环的函数
    palterimpl->shift(output, input);
}

}  // namespace ice
