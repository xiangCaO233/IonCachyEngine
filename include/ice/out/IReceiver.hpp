#pragma once

#include <memory>
#include <utility>

#include "ice/core/IAudioNode.hpp"
#include "ice/manage/AudioFormat.hpp"

namespace ice
{

/// @brief 音频输出接收端接口。
class IReceiver
{
public:
    /// @brief 构造接收端。
    /// @param format 接收端期望的音频格式。
    /// 基类不保存或验证该参数，格式状态由派生后端管理。
    explicit IReceiver(const AudioDataFormat& format);

    /// @brief 析构接收端。
    /// 基类默认析构不会调用 stop 或 close，不能用接口析构代替后端收尾契约。
    virtual ~IReceiver() = default;

    /// @brief 打开后端设备或文件。
    /// @return 成功时返回 true。
    virtual bool open() = 0;

    /// @brief 请求关闭后端，具体资源回收范围由实现决定。
    /// @warning 可能等待线程或执行文件收尾，只在低频生命周期路径调用。
    virtual void close() = 0;

    /// @brief 开始拉取数据并播放或写入。
    /// 设备后端可能启动工作线程，文件后端可能同步处理完整输出，不能统一假设立即返回。
    /// @return 成功时返回 true。
    virtual bool start() = 0;

    /// @brief 请求停止拉取，是否等待完成由具体后端规定。
    /// 返回不统一保证已关闭设备、排空编码尾部或回收工作线程。
    virtual void stop() = 0;

    /// @brief 查询接收端是否正在运行。
    /// @return 后端报告的运行状态，不是所有资源已释放或文件已持久化的统一证明。
    virtual bool is_running() const = 0;

    /// @brief 设置音频图最终输出节点。
    /// @param source 要从中拉取数据的音频节点。
    /// @warning
    /// 默认实现直接替换非原子的共享指针，必须在停止拉取时设置。
    /// 低频调用本身不能保证与播放线程并发读写安全。
    virtual void set_source(std::shared_ptr<IAudioNode> source)
    {
        // 转移共享所有权可能释放旧音频图，因此不能放在音频处理回调中。
        data_source = std::move(source);
    }

protected:
    /// @brief 获取当前音频图最终输出节点。
    /// @return 音频节点共享指针引用。
    /// @warning 音频热路径访问：返回引用以避免每个 buffer 复制 shared_ptr。
    /// 引用不额外保活源；接收端与成员必须存活，且不得与 set_source 并发。
    [[nodiscard]] virtual const std::shared_ptr<IAudioNode>& get_source() const
    {
        return data_source;
    }

private:
    /// @brief 当前音频图最终输出节点。
    std::shared_ptr<IAudioNode> data_source{ nullptr };
};

}  // namespace ice
