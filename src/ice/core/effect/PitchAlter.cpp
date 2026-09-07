#include <rubberband/RubberBandStretcher.h>

#include <ice/core/effect/PitchAlter.hpp>

namespace ice
{
/// @brief 拉取上游，单位倍率附近直通，否则交给变调器。
/// @warning 每块调用；历史实现仍会 resize 并复制上游 shared_ptr，尚未实时化。
void PitchAlter::process(AudioBuffer& buffer)
{
    // 准备输入缓冲区
    AudioBuffer& input_buf = get_inputbuffer();
    // 与基类固定容量路径不同，这里会按本次块长调整容量。
    input_buf.resize(buffer.afmt, buffer.num_frames());
    input_buf.clear();
    // 空上游不改写输出；调用方不能将本函数视为总会清零的音频源。
    if ( get_inputnode() ) {
        // 拉取数据
        get_inputnode()->process(input_buf);
        if ( std::abs(pitch_scale.load() - 1.) < 0.001f ) {
            // 直通使用累加而非覆盖，调用方需明确输出的初始内容。
            buffer += input_buf;
            return;
        } else {
            apply_effect(buffer, input_buf);
        }
    }
}
/// @brief 使用 RubberBand 包装处理平面 PCM，不改变目标时间倍率。
/// @warning 首次调用惰性创建并预热后端，不能视为无分配的音频热路径。
void PitchAlter::apply_effect(AudioBuffer& output, const AudioBuffer& input)
{
    // 后端沿用首次输入格式，后续更换格式没有在此触发自动重建。
    if ( !stretcher ) {
        stretcher = std::make_unique<RStretcher>(input.afmt);
    }

    // 倍率只在本次处理前设置，不应由别的线程直接修改同一后端实例。
    stretcher->set_pitch_ratio(pitch_scale.load());

    // 后端可能先消耗输入而暂不产出等长音频，不能假设输入输出一一对应。
    stretcher->process(output, input);
}
}  // namespace ice
