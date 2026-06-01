#ifndef ICE_MIXBUS_HPP
#define ICE_MIXBUS_HPP

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <set>

#include "ice/core/IAudioNode.hpp"
#include "ice/manage/AudioBuffer.hpp"

namespace ice
{
/// @brief 混音总线的双声道输出模式。
enum class MixBusChannelMode : uint8_t {
    Stereo = 0,       ///< 保持原始立体声输出。
    MuteLeft,         ///< 静音左声道，仅保留右声道。
    MuteRight,        ///< 静音右声道，仅保留左声道。
    CopyLeftToRight,  ///< 将左声道复制到右声道，两侧都播放左声道。
    CopyRightToLeft   ///< 将右声道复制到左声道，两侧都播放右声道。
};

/// @brief 混音总线，可汇总多个音频节点并输出到播放后端。
class MixBus : public IAudioNode
{
public:
    /// @brief 构造混音总线。
    MixBus() = default;

    /// @brief 析构混音总线。
    ~MixBus() override = default;

    /// @brief 处理并混合所有输入源。
    /// @param buffer 输出缓冲区。
    /// @warning 音频回调热路径：每个音频缓冲周期执行；禁止引入阻塞 I/O
    /// 或额外线程同步。
    void process(AudioBuffer& buffer) override;

    /// @brief 添加输入音频节点。
    /// @param src 输入音频节点。
    void add_source(std::shared_ptr<IAudioNode> src);

    /// @brief 移除输入音频节点。
    /// @param src 输入音频节点。
    void remove_source(std::shared_ptr<IAudioNode> src);

    /// @brief 清空全部输入音频节点。
    void clear()
    {
        std::lock_guard<std::mutex> lock(sources_mutex);
        sources.clear();
    }

    /// @brief 设置双声道输出模式。
    /// @param mode 目标输出模式。
    void set_channel_mode(MixBusChannelMode mode)
    {
        m_channelMode.store(static_cast<uint8_t>(mode),
                            std::memory_order_relaxed);
    }

    /// @brief 获取当前双声道输出模式。
    /// @return 当前输出模式。
    MixBusChannelMode get_channel_mode() const
    {
        return static_cast<MixBusChannelMode>(
            m_channelMode.load(std::memory_order_relaxed));
    }

    /// @brief 兼容旧接口：设置是否静音左声道。
    /// @param mute 是否静音左声道。
    void set_mute_left(bool mute)
    {
        if ( mute ) {
            set_channel_mode(MixBusChannelMode::MuteLeft);
        } else if ( get_channel_mode() == MixBusChannelMode::MuteLeft ) {
            set_channel_mode(MixBusChannelMode::Stereo);
        }
    }

    /// @brief 兼容旧接口：设置是否静音右声道。
    /// @param mute 是否静音右声道。
    void set_mute_right(bool mute)
    {
        if ( mute ) {
            set_channel_mode(MixBusChannelMode::MuteRight);
        } else if ( get_channel_mode() == MixBusChannelMode::MuteRight ) {
            set_channel_mode(MixBusChannelMode::Stereo);
        }
    }

    /// @brief 兼容旧接口：获取左声道是否处于静音模式。
    /// @return 左声道静音时返回 true。
    bool is_mute_left() const
    {
        return get_channel_mode() == MixBusChannelMode::MuteLeft;
    }

    /// @brief 兼容旧接口：获取右声道是否处于静音模式。
    /// @return 右声道静音时返回 true。
    bool is_mute_right() const
    {
        return get_channel_mode() == MixBusChannelMode::MuteRight;
    }

    /// @brief 获取左声道实时电平。
    /// @return 左声道电平。
    float get_left_level() const { return m_leftLevel.load(); }

    /// @brief 获取右声道实时电平。
    /// @return 右声道电平。
    float get_right_level() const { return m_rightLevel.load(); }

private:
    /// @brief 输入源列表。
    std::set<std::shared_ptr<ice::IAudioNode>> sources;

    /// @brief 保护 sources 的互斥锁。
    std::mutex sources_mutex;

    /// @brief 临时混音缓冲区。
    AudioBuffer temp_buffer;

    /// @brief 双声道输出模式。
    /// @warning 音频回调热路径读取；UI 线程写入，音频线程只需读取最新模式，
    /// 因此使用 relaxed 原子避免加锁。
    std::atomic<uint8_t> m_channelMode{ static_cast<uint8_t>(
        MixBusChannelMode::Stereo) };

    /// @brief 左声道电平。
    std::atomic<float> m_leftLevel{ 0.0f };

    /// @brief 右声道电平。
    std::atomic<float> m_rightLevel{ 0.0f };
};
}  // namespace ice

#endif  // ICE_MIXBUS_HPP
