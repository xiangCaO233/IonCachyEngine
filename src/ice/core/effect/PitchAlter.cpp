#include <rubberband/RubberBandStretcher.h>

#include <ice/core/effect/PitchAlter.hpp>

namespace ice
{
/// @brief 拉取上游，单位倍率附近直通，否则交给变调器。
/// @warning 每块调用；历史实现仍会 resize 并复制上游 shared_ptr，尚未实时化。
/// 上游配置应在处理停止时修改，当前共享复制并非已证明必需的跨线程生命周期方案。
/// @warning
/// 倍率由控制侧写、处理侧以默认顺序一致原子读取；不能据此认定整块参数一致。
void PitchAlter::process(AudioBuffer& buffer)
{
    // 覆盖了基类
    // process，因此没有复用基类对准备格式、最大帧数及上游改格式的检查。
    AudioBuffer& input_buf = get_inputbuffer();
    // 与基类固定容量路径不同，这里会按本次块长调整容量。
    input_buf.resize(buffer.afmt, buffer.num_frames());
    input_buf.clear();
    // 空上游不改写输出；调用方不能将本函数视为总会清零的音频源。
    if ( get_inputnode() ) {
        // 条件与调用各获取一次共享句柄；两次读取不是允许并发替换上游的同步协议。
        get_inputnode()->process(input_buf);
        if ( std::abs(pitch_scale.load() - 1.) < 0.001f ) {
            // 近似单位倍率选择旁路，不是只在精确等于一时绕过算法。
            // 此分支不清除已有后端状态，离开旁路后仍会使用之前的算法历史。
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
/// @warning
/// 每块再次读取控制侧倍率，使用默认顺序一致原子操作；值可与旁路判断时不同。
void PitchAlter::apply_effect(AudioBuffer& output, const AudioBuffer& input)
{
    // 后端沿用首次输入格式，后续更换格式没有在此触发自动重建。
    if ( !stretcher ) {
        stretcher = std::make_unique<RStretcher>(input.afmt);
    }

    // 倍率只在本次处理前设置，不应由别的线程直接修改同一后端实例。
    // 创建后端时没有在此使用调用方块长重新准备，不能把本接口当成任意容量适配层。
    stretcher->set_pitch_ratio(pitch_scale.load());

    // 后端可能先消耗输入而暂不产出等长音频，不能假设输入输出一一对应。
    stretcher->process(output, input);
}
}  // namespace ice
