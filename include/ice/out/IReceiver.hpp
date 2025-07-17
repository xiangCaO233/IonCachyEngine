#ifndef ICE_IRECEIVER_HPP
#define ICE_IRECEIVER_HPP

#include <memory>

#include "ice/core/IAudioNode.hpp"
#include "ice/manage/AudioFormat.hpp"

namespace ice {

class IReceiver {
   public:
    // 构造IReceiver
    explicit IReceiver(const AudioDataFormat &format);

    // 析构IReceiver
    virtual ~IReceiver() = default;

    // 状态管理
    // 打开后端设备或文件,进行所有必要的准备工作
    virtual bool open() = 0;

    // 关闭后端,释放所有资源
    virtual void close() = 0;

    // 开始拉取数据并播放/写入
    virtual bool start() = 0;

    // 停止拉取数据
    virtual void stop() = 0;

    // 查询状态
    virtual bool is_running() const = 0;

    // --- 数据源管理 ---
    // 设置要从中拉取数据的音频节点图的最终节点
    virtual void set_source(std::shared_ptr<IAudioNode> source) {
        data_source = source;
    }

   protected:
    [[nodiscard]] virtual std::shared_ptr<IAudioNode> get_source() const {
        return data_source;
    }

   private:
    // 数据来源
    std::shared_ptr<IAudioNode> data_source{nullptr};
};

}  // namespace ice

#endif  // ICE_IRECEIVER_HPP
