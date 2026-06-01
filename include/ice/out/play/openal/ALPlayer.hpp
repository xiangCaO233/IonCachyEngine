#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <ice/config/config.hpp>
#include <ice/manage/AudioBuffer.hpp>
#include <ice/out/IReceiver.hpp>

namespace ice
{

/// @brief OpenAL 输出设备信息。
struct ALAudioDeviceInfo {
    /// @brief 人类可读的设备名称。
    std::string name;
};

class ALBackend;

/// @brief 基于 OpenAL Soft 的实时播放后端。
class ALPlayer : public IReceiver
{
public:
    /// @brief 构造 OpenAL 播放器。
    /// @param format 从音频图拉取的内部音频格式。
    explicit ALPlayer(
        const AudioDataFormat& format = ICEConfig::internal_format);

    /// @brief 析构播放器并释放 OpenAL 资源。
    ~ALPlayer() override;

    /// @brief OpenAL 后端是否已经初始化。
    static std::atomic<bool> al_inited;

    /// @brief 初始化 OpenAL 后端全局状态。
    static bool init_backend();

    /// @brief 结束 OpenAL 后端全局状态。
    static void quit_backend();

    /// @brief 列出可用 OpenAL 输出设备。
    /// @return 可用输出设备列表，失败时返回空列表或默认设备。
    static std::vector<ALAudioDeviceInfo> list_devices();

    /// @brief 打开默认 OpenAL 输出设备。
    /// @return 成功时返回 true。
    bool open() override;

    /// @brief 打开指定 OpenAL 输出设备。
    /// @param deviceName list_devices 返回的设备名称；空字符串表示默认设备。
    /// @return 成功时返回 true。
    bool open(std::string_view deviceName);

    /// @brief Get the latest OpenAL backend failure detail.
    /// @return Last backend error message, or an empty string when no error is
    /// recorded.
    const std::string& getLastError() const;

    /// @brief Get the successfully opened OpenAL device name.
    /// @return Current device name, or an empty string when no device is open.
    const std::string& getOpenedDeviceName() const;

    /// @brief 关闭 OpenAL 设备并释放资源。
    void close() override;

    /// @brief 启动拉取音频数据的播放线程。
    /// @return 成功启动时返回 true。
    bool start() override;

    /// @brief 停止播放线程。
    void stop() override;

    /// @brief 查询播放线程是否运行中。
    bool is_running() const override;

    /// @brief 暂停 OpenAL 源播放。
    void pause();

    /// @brief 恢复 OpenAL 源播放。
    void resume();

    /// @brief 设置是否使用 OpenAL 空间化输出。
    /// @details 开启后会将混音结果下混为 mono，OpenAL 才能应用方向和距离衰减。
    /// @param enabled 是否开启空间化输出。
    void set_spatial_output_enabled(bool enabled);

    /// @brief 查询是否使用 OpenAL 空间化输出。
    bool is_spatial_output_enabled() const;

    /// @brief 设置 OpenAL 声源方向和距离参数。
    /// @param directionX 声源方向 X 分量。
    /// @param directionY 声源方向 Y 分量。
    /// @param directionZ 声源方向 Z 分量。
    /// @param distance 声源距离。
    /// @param referenceDistance 参考距离。
    /// @param maxDistance 最大距离。
    /// @param rolloffFactor 距离衰减倍率。
    void set_spatial_parameters(float directionX, float directionY,
                                float directionZ, float distance,
                                float referenceDistance, float maxDistance,
                                float rolloffFactor);

    /// @brief OpenAL 音频拉取线程入口。
    /// @warning 音频热路径：播放期间持续运行，禁止加入文件
    /// IO、长时间锁等待或高频日志。
    void audio_thread_loop();

private:
    /// @brief 将当前音频图数据写入 OpenAL buffer 并排队。
    /// @warning 音频热路径：由 OpenAL 播放线程调用，应避免额外分配和阻塞操作。
    bool queueAudioBuffer(unsigned int               bufferId,
                          std::vector<float>&        floatScratch,
                          std::vector<std::int16_t>& int16Scratch);

    /// @brief 清空当前 OpenAL 源上已经排队的 buffer。
    void clearQueuedBuffers();

    /// @brief 将缓存的空间化状态应用到 OpenAL 源。
    void applySpatialState();

    /// @brief 播放器拉取和提交音频时使用的格式。
    AudioDataFormat m_playFormat;

    /// @brief 播放线程内部复用的平面音频缓冲区。
    AudioBuffer m_buffer;

    /// @brief OpenAL 设备、上下文、源和 buffer 句柄。
    std::unique_ptr<ALBackend> m_backend;

    /// @brief Latest OpenAL backend failure detail for caller-side logging.
    std::string m_lastError;

    /// @brief Name of the successfully opened OpenAL playback device.
    std::string m_openedDeviceName;

    /// @brief 播放线程是否处于运行状态。
    std::atomic<bool> m_running{ false };

    /// @brief 播放线程是否处于暂停状态。
    std::atomic<bool> m_paused{ false };

    /// @brief 空间化输出是否启用。
    std::atomic<bool> m_spatialOutputEnabled{ false };

    /// @brief 队列格式是否需要在播放线程中重建。
    std::atomic<bool> m_rebuildQueuedBuffers{ false };

    /// @brief 保护音频图读取入口。
    std::mutex m_sourceMutex;

    /// @brief OpenAL 后端播放线程。
    std::thread m_audioThread;
};

}  // namespace ice
