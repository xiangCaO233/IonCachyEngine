#include <rubberband/RubberBandStretcher.h>

#include <atomic>
#include <cmath>
#include <ice/core/effect/TimeStretcher.hpp>
#include <memory>

namespace ice {
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
    if (!stretcher) {
        // 需要拉伸且不存在拉伸器-在此初始化
        stretcher = std::make_unique<RStretcher>(output.afmt);
    }
    // 更新拉伸倍率
    if (stretcher->get_stretch_ratio() != actual_stretch_ratio) {
        stretcher->set_stretch_ratio(actual_stretch_ratio.load());
    }

    // 执行拉伸
    stretcher->process(output, input);
}
}  // namespace ice
