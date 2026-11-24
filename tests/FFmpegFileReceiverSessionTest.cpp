#include "ice/config/config.hpp"
#include "ice/manage/AudioBuffer.hpp"
#include "ice/manage/dec/ffmpeg/FFmpegDecoderInstance.hpp"
#include "ice/out/io/FFmpegFileReceiver.hpp"

#include <cstddef>
#include <filesystem>
#include <string>
#include <system_error>

namespace
{
/// @brief 预算必须产生两个完整块及最后的短块。
constexpr std::size_t FRAME_BUDGET = 529;
/// @brief 固定尺寸用于验证每次打开都创建独立的输出 FIFO。
constexpr std::size_t BLOCK_FRAMES = 256;

/// @brief 打开刚写出的 PCM WAVE 并确认解码端看见预算范围内的全部帧。
/// @details 使用公开解码器读回产物，覆盖用户从磁盘重新打开文件的路径。
/// 不能只读输出文件大小，因为未排空的容器同样可能已有数据。
/// 定长 PCM WAVE 的输入和输出采样率一致，可以精确比较帧数。
/// @param path 上个编码会话关闭后的产物。
/// @return 预算、实际解码帧和最终读状态均符合要求时返回 true。
bool verifyOutput(const std::filesystem::path& path)
{
    const auto        u8 = path.u8string();
    const std::string name(reinterpret_cast<const char*>(u8.data()), u8.size());
    const auto        format = ice::ICEConfig::internal_format;
    ice::FFmpegDecoderInstance decoder(name, format);
    // 多读一帧能区分准确的短尾和多写一块；零且无错误才是正常曲尾。
    ice::AudioBuffer buffer(format, FRAME_BUDGET + 1);
    return decoder.isValid() && buffer.raw_ptrs() &&
           decoder.read(buffer.raw_ptrs(), FRAME_BUDGET + 1) == FRAME_BUDGET &&
           decoder.read(buffer.raw_ptrs(), 1) == 0 &&
           decoder.last_read_error() == 0;
}

/// @brief 验证半初始化失败及取消均保留诊断，接收端可重新开启会话。
/// @param directory CTest 提供的隔离目录，输出文件仅写入其中。
/// @return 零表示所有状态均能独立关闭和重试，非零标记失败阶段。
int verifySession(const std::filesystem::path& directory)
{
    const auto      parent = directory / "output";
    const auto      file   = parent / "test.wav";
    std::error_code error;
    // 清理本用例命名的文件，再移除空目录；不要递归影响工作目录中的其他产物。
    std::filesystem::remove(file, error);
    if ( error ) return 1;
    std::filesystem::remove(parent, error);
    if ( error ) return 1;

    ice::FFmpegFileReceiver receiver(file);
    // 预算计入源时钟下的静音帧，空源仍能驱动完整编码流程。
    receiver.set_target_frames(FRAME_BUDGET);
    receiver.set_block_frames(BLOCK_FRAMES);
    // 路径的父目录缺失，编码器等早期资源已经建立，但文件无法打开。
    if ( receiver.open() || receiver.error_message().empty() ) return 2;
    // 复制诊断文本，因为接收器只借出会随下一个 open 改变的字符串。
    const auto openFailure = receiver.error_message();
    receiver.close();
    // 重复 close 不覆盖原诊断，也不能让失败打开的会话进入排尾。
    if ( receiver.is_running() || receiver.error_message() != openFailure )
        return 3;
    // 只修复文件系统前置条件，保持同一个接收端继续验证失败后的复用。
    std::filesystem::create_directories(parent, error);
    if ( error || !receiver.open() || !receiver.error_message().empty() )
        return 4;

    // stop 在首块完成的同步回调中发出，计数应停留在已进入编码链的块。
    // 在同一线程发取消可精确定位块边界，不依赖线程调度或固定延时。
    // 取消后的错误文本在会话析构后仍由接收端持有。
    receiver.set_progress_callback([&](std::size_t) { receiver.stop(); });
    if ( receiver.start() || receiver.is_running() ||
         receiver.frames_written() != BLOCK_FRAMES ||
         receiver.error_message().find("stopped") == std::string::npos )
        return 5;
    // 保留已写的部分产物，让下一次打开走真实的原路径覆盖。
    receiver.close();

    // 使用同一接收端重复导出，验证新会话不继承取消状态、PTS 或诊断。
    receiver.set_progress_callback({});
    if ( !receiver.start() || receiver.is_running() ||
         receiver.frames_written() != FRAME_BUDGET ||
         !receiver.error_message().empty() || !verifyOutput(file) )
        return 6;
    // 第二次 close 必须是无操作，不能把完整容器再刷一次尾部。
    receiver.close();
    return verifyOutput(file) ? 0 : 7;
}
}  // namespace

/// @brief 从 CTest 传入独立输出目录，使测试不依赖音频设备和源码资源。
/// @param argc 包含程序名的参数数量，必须提供一个目录。
/// @param argv 目录使用平台文件系统路径，不硬编码工作目录。
/// @return 非零状态标识未通过的阶段。
int main(int argc, char** argv)
{
    if ( argc != 2 ) return 8;
    return verifySession(argv[1]);
}
