#pragma once

#include <ice/config/config.hpp>
#include <ice/manage/AudioBuffer.hpp>
#include <ice/manage/AudioFormat.hpp>
#include <ice/out/IReceiver.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

struct AVCodecContext;
struct AVFormatContext;
struct AVAudioFifo;
struct AVFrame;
struct AVPacket;
struct AVStream;
struct SwrContext;

namespace ice
{

/// @brief 使用 FFmpeg 编码器写入音频文件的离线接收端。
/// @details start 同步执行；除 stop 与运行标志查询外，对象配置、诊断和资源访问
/// 需要由调用者串行安排。错误或取消可能留下不完整文件，不提供事务式替换。
class FFmpegFileReceiver : public IReceiver
{
public:
    /// @brief 构造文件接收端。
    /// @param output_path 输出文件路径。
    /// @param format 引擎内部音频格式。
    explicit FFmpegFileReceiver(
        std::filesystem::path  output_path,
        const AudioDataFormat& format = ICEConfig::internal_format);

    /// @brief 析构并释放 FFmpeg 资源。
    /// @warning 可能编码排尾并执行文件 IO；必须等待 start
    /// 返回后再销毁，不提供线程 join。
    ~FFmpegFileReceiver() override;

    /// @brief 设置输入音频图的帧预算，不是编码器输出样本数。
    /// @param frame_count 按输入采样率计数的总帧数；零值会被 start 拒绝。
    /// @details 运行期间忽略修改，当前导出预算保持不变。
    /// @warning 配置路径：不能与 start 并发修改普通成员。
    void set_target_frames(std::size_t frame_count);

    /// @brief 设置每次拉取和编码的块大小。
    /// @param frame_count 输入块帧数；零值或超过 int 上界时保留原配置。
    /// @details 运行期间忽略修改，不使音源处理中的缓冲失效。
    /// @warning 配置路径：可能重新分配内存，不能与编码或图拉取并发。
    void set_block_frames(std::size_t frame_count);

    /// @brief 设置在 start 调用线程同步执行的输入进度回调。
    /// @param callback 参数为已送入编码链路的输入帧数，不保证已经写入文件。
    /// @details 运行期间忽略替换，防止回调销毁正在执行的自身。
    /// @warning 不允许并发替换；回调不得重入 close 或销毁正在执行 start
    /// 的对象。
    void set_progress_callback(std::function<void(std::size_t)> callback);

    /// @brief 获取已送入编码链路的累计输入帧数。
    /// @return 输入采样率下的帧计数，不是重采样输出数量或磁盘持久化进度。
    /// @warning 普通成员查询不提供跨线程同步；编码期间宜通过回调转发进度。
    [[nodiscard]] std::size_t frames_written() const;

    /// @brief 获取最近一次失败原因。
    /// @return 内部错误字符串的借用引用，后续操作可能覆盖内容。
    /// @warning 不能与错误更新、open 或对象销毁并发读取。
    [[nodiscard]] const std::string& error_message() const;

    /// @brief 打开输出文件和 FFmpeg 编码器。
    /// @return 成功时返回 true。
    /// @warning 离线文件操作：可能创建或截断已有文件，失败不会恢复原文件内容。
    bool open() override;

    /// @brief 尝试正常排尾并释放输出链路；已有错误时跳过补尾。
    /// 关闭错误通过 error_message 保存，但不覆盖已有编码错误或取消原因。
    /// @warning 离线耗时路径：可能编码与写文件，不能与 start
    /// 并发；不删除部分输出。
    void close() override;

    /// @brief 同步拉取 source 并写完整个目标文件。
    /// @return 输入预算、收尾及输出流关闭均成功时返回 true，取消同样返回
    /// false；进入编码循环后会在返回前关闭资源，前置检查失败则直接返回。
    /// @warning 离线耗时路径：会持续拉取音频图并编码文件，不能在音频实时线程或
    /// UI 热路径中调用。
    /// @warning 原子运行标志不是互斥获取，不支持多个线程并发 start。
    bool start() override;

    /// @brief 请求停止离线编码。
    /// @warning 跨线程协作取消：只发布标志，不等待退出，也不能中断当前编码、IO
    /// 或回调。
    /// @details 新 open 会清除旧停止标记；正常排尾阶段不持续检查停止请求。
    void stop() override;

    /// @brief 查询接收端是否正在离线编码。
    /// @return 最近发布的原子状态；false 不保证 close 已完成或资源可并发销毁。
    /// @warning 跨线程查询仅用于状态展示，安全回收仍须等待执行 start
    /// 的线程完成。
    [[nodiscard]] bool is_running() const override;

private:
    /// @brief 按输出路径扩展名初始化容器、编码器和输出流。
    /// @return 成功时返回 true。
    bool open_encoder();

    /// @brief 初始化输入到编码器格式的重采样器。
    /// @return 成功时返回 true。
    bool open_resampler();

    /// @brief 转换输入块并入 FIFO，提交已满足编码帧长的样本。
    /// @param buffer 引擎平面浮点缓冲。
    /// @param frame_count 有效帧数。
    /// @return 转换和入队成功时返回 true，不保证已经产生编码包。
    bool write_buffer(const AudioBuffer& buffer, std::size_t frame_count);

    /// @brief 将转换后的音频样本写入 FIFO。
    /// @param converted_data 转换后的声道数据。
    /// @param frame_count 转换后的帧数。
    /// @return 完整入队或空输入时返回 true；正帧缺资源或负帧数返回 false。
    /// @details 复制样本但不接管输入数组所有权，帧数按输出采样率计算。
    bool write_converted_to_fifo(uint8_t** converted_data, int frame_count);

    /// @brief 将 FIFO 中足够编码的样本送入编码器。
    /// @param flush 是否正在结束编码，允许写出不足一整帧的尾部。
    /// @details 短尾不在此补零；发送失败不回滚已取出的 FIFO 数据和 PTS。
    /// @return 成功时返回 true。
    bool encode_fifo(bool flush);

    /// @brief 依次排出转换延迟、FIFO 尾部和编码器包，再写容器 trailer。
    /// @warning 离线收尾不检查停止标记，失败不能无条件重试或回滚输出。
    /// @return 成功时返回 true。
    bool finish_encoding();

    /// @brief 发送一帧给编码器。
    /// @param frame FFmpeg 音频帧；nullptr 表示刷新编码器。
    /// @details 发送成功后立即取包；send 返回 EAGAIN
    /// 也按失败处理，没有重试分支。
    /// @return 成功时返回 true。
    bool send_frame(AVFrame* frame);

    /// @brief 接收并写出编码包。
    /// @param draining 已发送结束帧时为 true，要求最终观察到 EOF。
    /// @return 普通取包遇到 EAGAIN 或最终排空遇到 EOF 时成功，否则报告错误。
    bool drain_packets(bool draining);

    /// @brief 记录 FFmpeg 错误文本。
    /// @param prefix 错误上下文。
    /// @param code FFmpeg 错误码。
    void set_ffmpeg_error(const char* prefix, int code);

    /// @brief 记录普通错误文本。
    /// @param message 错误文本。
    void set_error(std::string message);

    /// @brief 输出文件路径。
    std::filesystem::path m_outputPath;

    /// @brief 引擎输入音频格式。
    AudioDataFormat m_format;

    /// @brief 离线拉取缓冲。
    AudioBuffer m_buffer;

    /// @brief 需要从音频图拉取的目标帧数。
    std::size_t m_targetFrames{ 0 };

    /// @brief 每次处理的帧数。
    std::size_t m_blockFrames{ 65536 };

    /// @brief 已从音频图拉取并送入编码链路的帧数。
    /// @details 在输入时钟下累计，close 后保留供查询，新 open
    /// 时清零；不与写线程外同步。
    std::size_t m_framesWritten{ 0 };

    /// @brief 进度回调。
    /// @details 在编码线程执行，不自动投递
    /// UI；其捕获对象必须覆盖每次同步通知的生命周期。
    std::function<void(std::size_t)> m_progressCallback;

    /// @brief 最近一次错误文本。
    std::string m_errorMessage;

    /// @brief 离线编码运行标记。
    /// @warning 低频跨线程查询标记；仅由 start/close 写入，UI 或任务线程读取。
    /// @details 原子避免状态读写的数据竞争，不保护普通成员；start 在 close
    /// 前即可清除此值。所有访问使用 relaxed，不发布诊断或资源生命周期。
    std::atomic<bool> m_running{ false };

    /// @brief 离线编码停止请求。
    /// @warning stop 跨线程写 true，open 清 false，start
    /// 每离线块读取；用于协作取消而非阻塞同步。
    /// @details relaxed 足以传递独立布尔请求，不依赖其他数据的先行发布。
    /// 对象生命周期与配置串行化仍由调用者保证，不能从取消已发送推断退出。
    std::atomic<bool> m_stopRequested{ false };

    /// @brief FFmpeg 输出容器上下文。
    /// @details 由接收端拥有，close 关闭所需 AVIO 并释放容器及其流对象。
    AVFormatContext* m_formatContext{ nullptr };

    /// @brief FFmpeg 音频编码器上下文。
    AVCodecContext* m_codecContext{ nullptr };

    /// @brief 容器拥有的音频流观察指针，容器释放后仅清空，不单独释放。
    AVStream* m_stream{ nullptr };

    /// @brief 复用的编码输出包。
    AVPacket* m_packet{ nullptr };

    /// @brief 引擎格式到编码器格式的重采样器。
    SwrContext* m_swrContext{ nullptr };

    /// @brief 缓存重采样后样本并按编码器帧长输出的 FIFO。
    AVAudioFifo* m_fifo{ nullptr };

    /// @brief 下一个编码帧的 PTS，单位为实际编码采样率的一个样本周期。
    /// @details 发送前即推进，不等同于成功写包的时长，也不用于输入进度回调。
    int64_t m_nextPts{ 0 };

    /// @brief 输出链路是否已打开。
    bool m_opened{ false };

    /// @brief trailer 是否已经写出。
    /// @details
    /// 只在写尾成功后置位，用于防止重复正常收尾，不表示文件已完成磁盘持久化。
    bool m_trailerWritten{ false };
};

}  // namespace ice
