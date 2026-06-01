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
    explicit IReceiver(const AudioDataFormat& format);

    /// @brief 析构接收端。
    virtual ~IReceiver() = default;

    /// @brief 打开后端设备或文件。
    /// @return 成功时返回 true。
    virtual bool open() = 0;

    /// @brief 关闭后端并释放所有资源。
    virtual void close() = 0;

    /// @brief 开始拉取数据并播放或写入。
    /// @return 成功时返回 true。
    virtual bool start() = 0;

    /// @brief 停止拉取数据。
    virtual void stop() = 0;

    /// @brief 查询接收端是否正在运行。
    virtual bool is_running() const = 0;

    /// @brief 设置音频图最终输出节点。
    /// @param source 要从中拉取数据的音频节点。
    /// @warning
    /// 播放线程可能读取该指针；调用方应在低频路径设置，避免播放热路径频繁改动。
    virtual void set_source(std::shared_ptr<IAudioNode> source)
    {
        data_source = std::move(source);
    }

protected:
    /// @brief 获取当前音频图最终输出节点。
    /// @return 音频节点共享指针引用。
    /// @warning 音频热路径访问：返回引用以避免每个 buffer 复制 shared_ptr。
    [[nodiscard]] virtual const std::shared_ptr<IAudioNode>& get_source() const
    {
        return data_source;
    }

private:
    /// @brief 当前音频图最终输出节点。
    std::shared_ptr<IAudioNode> data_source{ nullptr };
};

}  // namespace ice
