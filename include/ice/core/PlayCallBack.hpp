#ifndef ICE_PLAYCALLBACK_HPP
#define ICE_PLAYCALLBACK_HPP

#include <chrono>
#include <cstddef>

namespace ice
{
class PlayCallBack
{
public:
    // 构造PlayCallBack
    PlayCallBack() = default;
    // 析构PlayCallBack
    virtual ~PlayCallBack() = default;

    // 播放完成完整一遍回调(传入是否循环)
    virtual void play_done(bool loop) const = 0;

    // 帧基
    virtual void frameplaypos_updated(size_t frame_pos) = 0;

    // 时间基
    virtual void timeplaypos_updated(std::chrono::nanoseconds time_pos) = 0;
};
}  // namespace ice

#endif  // ICE_PLAYCALLBACK_HPP
