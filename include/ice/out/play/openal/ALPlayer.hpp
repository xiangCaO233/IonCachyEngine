#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <ice/config/config.hpp>
#include <ice/manage/AudioBuffer.hpp>
#include <ice/out/IReceiver.hpp>

/// @brief SDL 线程句柄，具体定义只在播放实现中使用。
struct SDL_Thread;

namespace ice
{

/// @brief OpenAL 输出设备信息。
struct ALAudioDeviceInfo {
    /// @brief 人类可读的设备名称。
    std::string name;
};

class ALBackend;

/// @brief 基于 OpenAL Soft 的实时播放后端。
/// 生命周期由控制侧串行管理：设置音频图、打开设备、启动、停止、关闭。
/// 打开设备要求 ALC_EXT_thread_local_context 扩展及其入口可用。
/// 控制操作恢复调用线程原绑定，供数线程在整个运行期保留自己的绑定。
/// 基类 set_source 没有与供数线程共享写锁，必须在停止拉取期间调用。
/// 内部互斥量只串行化部分设备操作，不使整个公开接口具备并发调用安全性。
class ALPlayer : public IReceiver
{
public:
    /// @brief 构造 OpenAL 播放器。
    /// @param format 从音频图拉取的内部音频格式。
    explicit ALPlayer(
        const AudioDataFormat& format = ICEConfig::internal_format);

    /// @brief 析构播放器并释放 OpenAL 资源。
    /// @warning 包含线程 join 与设备关闭，只能在低频控制侧执行。
    ~ALPlayer() override;

    /// @brief 全局默认值初始化标记，不表示设备可用或播放已启动。
    /// @warning 控制侧初始化/退出写入，外部可查询；不保护实例资源生命周期。
    static std::atomic<bool> al_inited;

    /// @brief 初始化 OpenAL 后端全局状态。
    static bool init_backend();

    /// @brief 结束 OpenAL 后端全局状态。
    static void quit_backend();

    /// @brief 列出可用 OpenAL 输出设备。
    /// @return 可用输出设备列表，失败时返回空列表或默认设备。
    static std::vector<ALAudioDeviceInfo> list_devices();

    /// @brief 打开默认 OpenAL 输出设备。
    /// 默认设备失败后依次尝试枚举设备，成功名称可通过查询接口获取。
    /// @return 成功时返回 true。
    bool open() override;

    /// @brief 打开指定 OpenAL 输出设备。
    /// @param deviceName list_devices 返回的设备名称；空字符串表示默认设备。
    /// 含内嵌空字符的名称返回失败，并保留原设备及播放状态。
    /// @return 成功时返回 true。
    bool open(std::string_view deviceName);

    /// @brief 借用最近一次控制侧设备操作的错误详情。
    /// @return 无错误记录时为空；供数错误在 stop 后可读，已有控制侧错误优先。
    /// @warning 不得与 open/start/stop/close
    /// 并发访问，引用内容可能被后续操作改写。
    const std::string& getLastError() const;

    /// @brief 借用成功打开的实际设备名，可能不同于默认设备请求名。
    /// @return 未打开设备时为空；生命周期操作会改变引用内容。
    const std::string& getOpenedDeviceName() const;

    /// @brief 关闭 OpenAL 设备并释放资源。
    /// @warning 先 stop/join 再回收设备；不能在供数线程自身调用。
    void close() override;

    /// @brief 启动拉取音频数据的播放线程。
    /// @warning 控制侧低频调用，故障后重启会先 join 已退出或正在收尾的线程。
    /// @return 成功发起线程时返回 true；创建失败返回 false 并清除运行标记。
    /// 线程发起成功不表示首批数据排队已经成功。
    bool start() override;

    /// @brief 停止播放线程。
    /// @warning join 等待当前图处理和驱动调用完成，仅供低频控制流程使用。
    /// 保留设备和图绑定，允许后续重新 start。
    void stop() override;

    /// @brief 查询播放线程是否运行中。
    /// @warning 高频读取控制侧写入的原子请求；不是源播放状态或 join 完成通知。
    bool is_running() const override;

    /// @brief 暂停 OpenAL 源播放。
    /// 保留已排队块；当前供数迭代可能尚未观察到暂停，不能视作同步屏障。
    void pause();

    /// @brief 恢复 OpenAL 源播放。
    void resume();

    /// @brief 设置是否使用 OpenAL 空间化输出。
    /// @details 开启后会将混音结果下混为 mono，OpenAL 才能应用方向和距离衰减。
    /// 队列重建由供数线程异步消费，丢弃旧块但不回退音频图游标。
    /// @param enabled 是否开启空间化输出。
    void set_spatial_output_enabled(bool enabled);

    /// @brief 查询是否使用 OpenAL 空间化输出。
    /// @warning 高频读取控制侧原子配置，只反映请求值，不证明格式切换已完成。
    bool is_spatial_output_enabled() const;

    /// @brief 设置 OpenAL 声源方向和距离参数。
    /// @param directionX 声源方向 X 分量。
    /// @param directionY 声源方向 Y 分量。
    /// @param directionZ 声源方向 Z 分量。
    /// @param distance 声源距离。
    /// @param referenceDistance 参考距离。
    /// @param maxDistance 最大距离。
    /// @param rolloffFactor 距离衰减倍率。
    /// 非有限输入整组拒绝，保留原空间缓存与播放状态，并通过 getLastError
    /// 提供原因。
    void set_spatial_parameters(float directionX, float directionY,
                                float directionZ, float distance,
                                float referenceDistance, float maxDistance,
                                float rolloffFactor);

    /// @brief OpenAL 音频拉取线程入口。
    /// @warning 音频热路径：由 start 创建的专用线程持续执行，不得另行直接调用。
    /// 控制侧写入运行、暂停及模式原子标志，本线程读取并消费重建请求。
    /// 现有互斥锁和 1ms 供数轮询休眠仍可能阻塞；禁止加入文件 IO 或逐块日志。
    void audio_thread_loop();

private:
    /// @brief 将当前音频图数据写入 OpenAL buffer 并排队。
    /// @warning 音频热路径：由 OpenAL 播放线程调用，应避免额外分配和阻塞操作。
    /// 使用供数线程在空队列填充时确定的模式，控制侧请求不会逐槽改变布局。
    /// 既有失败诊断会格式化并输出，不能将错误路径视为实时安全。
    bool queueAudioBuffer(unsigned int               bufferId,
                          std::vector<float>&        floatScratch,
                          std::vector<std::int16_t>& int16Scratch);

    /// @brief 清空当前 OpenAL 源上已经排队的 buffer。
    /// @pre 已停源并持有后端 AL 锁；只退队，不删除缓冲对象。
    void clearQueuedBuffers();

    /// @brief 在线程回收后收集延迟错误，关闭期间的新错误也须在返回前收集。
    /// @warning 控制侧低频路径，内部获取后端锁；调用方不得已持有该锁。
    void collectPendingError();

    /// @brief 将缓存的空间化状态应用到 OpenAL 源。
    /// @return 无设备或成功时为 true；失败保存诊断并停止供数。
    bool applySpatialState();

    /// @brief 播放器拉取和提交音频时使用的格式。
    AudioDataFormat m_playFormat;

    /// @brief 播放线程内部复用的平面音频缓冲区。
    AudioBuffer m_buffer;

    /// @brief OpenAL 设备、上下文、源和 buffer 句柄。
    std::unique_ptr<ALBackend> m_backend;

    /// @brief 控制侧操作诊断，供调用方统一记录；不受原子或互斥量保护。
    std::string m_lastError;

    /// @brief 成功打开的设备实际名称，由控制侧生命周期操作更新。
    std::string m_openedDeviceName;

    /// @brief 播放线程是否处于运行状态。
    /// @warning 控制侧 start/stop 与供数失败路径写入，供数循环和外部查询读取。
    /// 失败清零只发布退出要求，控制侧仍需 join 才能回收资源。
    /// relaxed 只传递继续运行或停止请求，不发布设备、缓冲或错误文本。
    /// 跨线程轮询必须避免数据竞争；资源发布依靠线程创建，回收依靠等待线程结束。
    /// 生命周期调用仍须串行，不能观察到 false 后直接销毁后端。
    std::atomic<bool> m_running{ false };

    /// @brief 播放线程是否处于暂停状态。
    /// @warning 控制侧写入、供数循环读取，用于跳过后续供数；不是迭代完成屏障。
    std::atomic<bool> m_paused{ false };

    /// @brief 空间化输出是否启用。
    /// @warning 控制侧写入、供数与查询读取，决定下混布局；原子值避免数据竞争。
    std::atomic<bool> m_spatialOutputEnabled{ false };

    /// @brief 队列格式是否需要在播放线程中重建。
    /// @warning
    /// 控制侧置位，供数侧交换清零；合并请求以使用最新模式，不累计次数。
    std::atomic<bool> m_rebuildQueuedBuffers{ false };

    /// @brief 在实现文件中适配 SDL 调用约定的供数线程入口。
    struct WorkerEntry;
    /// @brief OpenAL 供数线程句柄，由串行控制侧创建并在等待结束后清空。
    /// @warning 线程借用 this；设备和实例必须活过 SDL_WaitThread 的低频等待。
    SDL_Thread* m_audioThread{ nullptr };
};

}  // namespace ice
