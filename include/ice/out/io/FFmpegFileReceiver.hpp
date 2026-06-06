#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <ice/config/config.hpp>
#include <ice/manage/AudioBuffer.hpp>
#include <ice/out/IReceiver.hpp>
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
    ~FFmpegFileReceiver() override;

    /// @brief 设置目标写入帧数。
    /// @param frame_count 需要从 source 拉取并编码的总帧数。
    void set_target_frames(std::size_t frame_count);

    /// @brief 设置每次拉取和编码的块大小。
    /// @param frame_count 块帧数。
    void set_block_frames(std::size_t frame_count);

    /// @brief 设置写入进度回调。
    /// @param callback 参数为已写入帧数。
    void set_progress_callback(std::function<void(std::size_t)> callback);

    /// @brief 获取已成功写入的帧数。
    /// @return 已写入帧数。
    [[nodiscard]] std::size_t frames_written() const;

    /// @brief 获取最近一次失败原因。
    /// @return 错误文本。
    [[nodiscard]] const std::string& error_message() const;

    /// @brief 打开输出文件和 FFmpeg 编码器。
    /// @return 成功时返回 true。
    bool open() override;

    /// @brief 写入 trailer 并关闭输出文件。
    void close() override;

    /// @brief 同步拉取 source 并写完整个目标文件。
    /// @return 成功时返回 true。
    /// @warning 离线耗时路径：会持续拉取音频图并编码文件，不能在音频实时线程或
    /// UI 热路径中调用。
    bool start() override;

    /// @brief 请求停止离线编码。
    void stop() override;

    /// @brief 查询接收端是否正在离线编码。
    /// @return 正在编码时返回 true。
    [[nodiscard]] bool is_running() const override;

private:
    /// @brief 按输出路径扩展名初始化容器、编码器和输出流。
    /// @return 成功时返回 true。
    bool open_encoder();

    /// @brief 初始化输入到编码器格式的重采样器。
    /// @return 成功时返回 true。
    bool open_resampler();

    /// @brief 写入一块音频。
    /// @param buffer 引擎平面浮点缓冲。
    /// @param frame_count 有效帧数。
    /// @return 成功时返回 true。
    bool write_buffer(const AudioBuffer& buffer, std::size_t frame_count);

    /// @brief 将转换后的音频样本写入 FIFO。
    /// @param converted_data 转换后的声道数据。
    /// @param frame_count 转换后的帧数。
    /// @return 成功时返回 true。
    bool write_converted_to_fifo(uint8_t** converted_data, int frame_count);

    /// @brief 将 FIFO 中足够编码的样本送入编码器。
    /// @param flush 是否正在结束编码，允许写出不足一整帧的尾部。
    /// @return 成功时返回 true。
    bool encode_fifo(bool flush);

    /// @brief 刷新重采样器和编码器。
    /// @return 成功时返回 true。
    bool finish_encoding();

    /// @brief 发送一帧给编码器。
    /// @param frame FFmpeg 音频帧；nullptr 表示刷新编码器。
    /// @return 成功时返回 true。
    bool send_frame(AVFrame* frame);

    /// @brief 接收并写出编码包。
    /// @return 成功时返回 true。
    bool drain_packets();

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
    std::size_t m_framesWritten{ 0 };

    /// @brief 进度回调。
    std::function<void(std::size_t)> m_progressCallback;

    /// @brief 最近一次错误文本。
    std::string m_errorMessage;

    /// @brief 离线编码运行标记。
    /// @warning 低频跨线程查询标记；仅由 start/close 写入，UI 或任务线程读取。
    std::atomic<bool> m_running{ false };

    /// @brief 离线编码停止请求。
    /// @warning 低频跨线程停止标记；由 stop 写入，start 的离线循环读取。
    std::atomic<bool> m_stopRequested{ false };

    /// @brief FFmpeg 输出容器上下文。
    AVFormatContext* m_formatContext{ nullptr };

    /// @brief FFmpeg 音频编码器上下文。
    AVCodecContext* m_codecContext{ nullptr };

    /// @brief FFmpeg 输出音频流。
    AVStream* m_stream{ nullptr };

    /// @brief 复用的编码输出包。
    AVPacket* m_packet{ nullptr };

    /// @brief 引擎格式到编码器格式的重采样器。
    SwrContext* m_swrContext{ nullptr };

    /// @brief 缓存重采样后样本并按编码器帧长输出的 FIFO。
    AVAudioFifo* m_fifo{ nullptr };

    /// @brief 下一个编码帧的 PTS。
    int64_t m_nextPts{ 0 };

    /// @brief 输出链路是否已打开。
    bool m_opened{ false };

    /// @brief trailer 是否已经写出。
    bool m_trailerWritten{ false };
};

}  // namespace ice
