#ifndef ICE_SOURCENODE_HPP
#define ICE_SOURCENODE_HPP

#include <atomic>
#include <chrono>
#include <ice/manage/AudioTrack.hpp>
#include <memory>
#include <set>

#include "ice/config/config.hpp"
#include "ice/core/IAudioNode.hpp"
#include "ice/core/PlayCallBack.hpp"

namespace ice {
class Resampler;
class SourceNode : public IAudioNode {
   public:
    // 构造SourceNode
    explicit SourceNode(std::shared_ptr<AudioTrack> track);
    // 析构SourceNode
    ~SourceNode() override;

    // 只管从播放位置读取请求的数据量并填充缓冲区
    void process(AudioBuffer& buffer) override;

    // 音源基本控制

    // 是否正在播放
    inline bool isplaying() { return is_playing.load(); }

    // 设置音源为暂停
    inline void pause() { is_playing.store(false); }

    // 设置音源为播放
    inline void play() { is_playing.store(true); }

    // 获取音源音量
    inline float getvolume() const { return volume.load(); }

    // 设置音源音量
    inline void setvolume(float v) { volume.store(v); }

    // 设置是否循环播放
    inline void setloop(bool flag) { is_looping.store(flag); }

    // 是否正在循环
    inline bool isloop() { return is_looping.load(); }

    // 获取播放位置-帧位置
    inline size_t get_playpos() const { return playback_pos.load(); }

    // 设置播放位置
    // 根据帧位置
    inline void set_playpos(size_t frame_pos) { playback_pos.store(frame_pos); }

    // 添加回调
    inline void add_playcallback(
        const std::shared_ptr<PlayCallBack>& callback) {
        callbacks.insert(callback);
    }

    // 移除回调
    inline void remove_playcallback(
        const std::shared_ptr<PlayCallBack>& callback) {
        callbacks.erase(callback);
    }

    // 根据时间(us)
    /**
     * @brief 根据一个时间段来设置播放位置。
     * 这是一个模板函数，能够接受任何类型的 std::chrono::duration
     * (例如：std::chrono::seconds, std::chrono::milliseconds,
     * std::chrono::nanoseconds 等)。
     *
     * 用法示例:
     * @code
     *   using namespace std::chrono_literals; 启用 s, ms, us, ns 等时间后缀
     *
     *   source_node->set_playpos(10s);
     * 跳转到第10秒 source_node->set_playpos(std::chrono::minutes(1) + 30s);
     * 跳转到1分30秒 source_node->set_playpos(500ms); 跳转到500毫秒
     * @endcode
     *
     * @tparam Rep    时间段的底层表示类型 (例如 int, double)。
     * @tparam Period 一个 std::ratio 类型，代表时间刻度的周期 (例如
     * std::ratio<1, 1000> 代表毫秒)。
     * @param time_pos 从音轨起始点开始计算的时间段。
     */
    template <typename Rep, typename Period>
    inline void set_playpos(
        const std::chrono::duration<Rep, Period>& time_pos) {
        // 获取音轨的采样率
        const auto sample_rate =
            static_cast<double>(track->get_media_info().format.samplerate);
        if (sample_rate == 0) {
            return;
        }

        // 将任何传入的时间单位，都安全地、精确地转换为“秒”
        // 我们使用一个以 double 为基础的 duration
        // 类型来接收转换结果，以保证最高精度。
        using double_seconds = std::chrono::duration<double>;
        const auto seconds =
            std::chrono::duration_cast<double_seconds>(time_pos).count();

        // 计算对应的帧位置
        const auto frame_position = static_cast<size_t>(seconds * sample_rate);

        // 安全地设置播放位置 (边界检查)
        const auto total_frames = track->get_media_info().frame_count;
        const auto final_position = std::min(frame_position, total_frames);

        // 原子地存储
        set_playpos(final_position);
    }

    // 获取音轨总帧数
    inline size_t num_frames() const {
        return track->get_media_info().frame_count;
    }

    // 获取音轨原始格式
    inline const AudioDataFormat& format() const {
        return track->get_media_info().format;
    }

    /**
     * @brief 获取音轨的总时长。
     *
     * 该函数基于音轨的总帧数和采样率进行精确计算，
     * 并以 std::chrono::nanoseconds 的形式返回，以提供最高精度。
     * 如果音轨信息无效（如采样率为0），则返回0。
     *
     * @return std::chrono::nanoseconds 表示总时长的对象。
     */
    [[nodiscard]] inline std::chrono::nanoseconds total_time() const {
        // 步骤 1: 获取总帧数和采样率
        const size_t total_frames_val = num_frames();
        const auto sample_rate =
            static_cast<double>(track->get_media_info().format.samplerate);

        // 防御性检查
        if (sample_rate == 0 || total_frames_val == 0) {
            return std::chrono::nanoseconds(0);
        }

        // 计算总时长（以秒为单位的浮点数）
        // 总时长 = 总帧数 / (帧/秒)
        const double duration_in_seconds =
            double(total_frames_val) / sample_rate;

        // 将这个浮点秒数转换为纳秒
        // 这是最关键、最能体现 std::chrono 威力的一步。

        // 创建一个以 double 为基础的秒单位的 duration 对象
        using double_seconds = std::chrono::duration<double>;
        const auto duration_fp = double_seconds(duration_in_seconds);

        // 使用 std::chrono::duration_cast 将其安全地转换为纳秒
        // duration_cast 会处理所有单位换算，并进行截断
        // 从而得到一个整数类型的纳秒值。
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            duration_fp);
    }

   private:
    // 重采样实现
    std::unique_ptr<Resampler> resampleimpl{nullptr};

    // 轨道指针
    std::shared_ptr<AudioTrack> track;

    // 播放回调列表
    std::set<std::shared_ptr<PlayCallBack>> callbacks;

    // 播放位置
    std::atomic<double> playback_pos{0.};

    // 音源音量
    std::atomic<float> volume{0.4f};

    // 音源是否循环(到结尾是否自动置零播放位置)
    std::atomic<bool> is_looping{false};

    // 音源是否正在播放
    std::atomic<bool> is_playing{false};

    // 应用音量到缓冲区
    void apply_volume(AudioBuffer& buffer) const;
};
}  // namespace ice

#endif  // ICE_SOURCENODE_HPP
