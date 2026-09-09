#include <ice/core/effect/PitchAlter.hpp>

#include <ice/core/IAudioNode.hpp>
#include <ice/core/effect/rubberband/RStretcher.hpp>
#include <ice/manage/AudioBuffer.hpp>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>

namespace ice
{
namespace
{
/// @brief 检查上游缓冲是否仍满足当前输出块的格式、长度及平面要求。
/// @warning 每块调用两次，仅遍历声道表，不分配或增加共享引用计数。
/// 非空悬空指针无法在此识别，上游仍须遵守存储生命周期约束。
bool hasMatchingInput(const AudioBuffer& input, const AudioBuffer& output)
{
    if ( input.afmt != output.afmt ||
         input.num_frames() != output.num_frames() ||
         input.frame_capacity() < output.num_frames() )
        return false;
    // 空块无需访问平面，允许默认空缓冲没有地址表。
    if ( output.num_frames() == 0 ) return true;
    const auto planes = input.raw_ptrs();
    if ( !planes ) return false;
    for ( uint16_t ch = 0; ch < input.num_channels(); ++ch ) {
        if ( !planes[ch] ) return false;
    }
    return true;
}
}  // namespace

/// @brief 拉取上游，单位倍率附近直通，否则交给变调器。
/// @warning 每块调用；历史实现仍会 resize，尚未实时化。
/// 上游配置只在处理停止时修改；节点成员持有所有权，本次调用仅借用。
/// @warning
/// 倍率由控制侧写、处理侧以默认顺序一致原子读取；不能据此认定整块参数一致。
void PitchAlter::process(AudioBuffer& buffer)
{
    // 覆盖基类 process 后自行校验上游块，容量准备仍沿用历史动态路径。
    AudioBuffer& input_buf = get_inputbuffer();
    // 与基类固定容量路径不同，这里会按本次块长调整容量。
    if ( !input_buf.resize(buffer.afmt, buffer.num_frames()) ||
         !hasMatchingInput(input_buf, buffer) ) {
        // 准备失败不能继续按旧块长拉取上游，否则会推进无法交付的音频位置。
        // 用静音覆盖当前输出，避免调用方误用未更新的旧样本。
        buffer.clear();
        return;
    }
    input_buf.clear();
    // 空上游不改写输出；调用方不能将本函数视为总会清零的音频源。
    if ( IAudioNode* input = get_inputnode_observer() ) {
        // 基类成员在整个处理期间保持上游存活，不为每个块增减共享引用计数。
        input->process(input_buf);
        // 上游不得改变块契约，先拒绝异常状态，再进入累加或后端样本读取。
        if ( !hasMatchingInput(input_buf, buffer) ) {
            buffer.clear();
            return;
        }
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
