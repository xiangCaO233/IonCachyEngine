#pragma once

#include <cstddef>
#include <exception>
#include <expected>

namespace ice
{
/// @brief 完整缓存未能获得可靠 PCM 时的准备诊断。
/// 由工作线程与缓存数据一起发布；可跨线程按值查询，不借用解码器内部存储。
struct CachedDecodeError {
    /// @brief 将失败定位到具体阶段，以便调用方选择重试或报告。
    enum class Stage {
        /// @brief 媒体探测之后仍未能建立独立解码实例。
        Open,
        /// @brief 后端输出格式不符合缓存要求。
        Format,
        /// @brief 无法建立从文件首帧顺序解码所需的状态。
        Seek,
        /// @brief 读取或转换 PCM 时遇到不可恢复的后端错误。
        Read,
        /// @brief 后端报告的有效帧数超出请求容量。
        FrameCount,
        /// @brief 后台任务没有交付结果或以异常结束。
        BackgroundTask
    };

    /// @brief 发生失效的阶段，成功结果不携带诊断。
    Stage stage;
    /// @brief 可用于后端诊断的原始错误码；零表示无附带错误码。
    int backendCode{};
    /// @brief 后台任务实际交付的异常，可由非实时错误报告调用方进一步检查。
    std::exception_ptr exception{};
};

/// @brief 成功返回可交付帧数（允许为零），失败保留原因。
using CachedDecodeResult = std::expected<std::size_t, CachedDecodeError>;

}  // namespace ice
