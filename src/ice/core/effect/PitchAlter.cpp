#include <rubberband/RubberBandStretcher.h>

#include <ice/core/effect/PitchAlter.hpp>

namespace ice
{
void PitchAlter::process(AudioBuffer& buffer)
{
    // 准备输入缓冲区
    AudioBuffer& input_buf = get_inputbuffer();
    input_buf.resize(buffer.afmt, buffer.num_frames());
    input_buf.clear();
    if ( get_inputnode() )
        {
            // 拉取数据
            get_inputnode()->process(input_buf);
            if ( std::abs(pitch_scale.load() - 1.) < 0.001f )
                {
                    // 直通优化
                    buffer += input_buf;
                    return;
                }
            else
                {
                    apply_effect(buffer, input_buf);
                }
        }
}
// 应用变调器
void PitchAlter::apply_effect(AudioBuffer& output, const AudioBuffer& input)
{
    // 惰性初始化
    if ( !stretcher )
        {
            stretcher = std::make_unique<RStretcher>(input.afmt);
        }

    stretcher->set_pitch_ratio(pitch_scale.load());

    // 实时处理
    stretcher->process(output, input);
}
}  // namespace ice
