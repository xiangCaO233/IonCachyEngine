#pragma once

#include <cstddef>
#include <memory>

#include "ice/core/IAudioNode.hpp"
#include "ice/manage/AudioBuffer.hpp"

namespace ice
{
/// @brief 管理上游缓冲准备与逐块拉取的效果节点基类。
/// @details 一个实例复用一块输入缓冲，不支持多个音频线程同时 process。
/// 基类不负责图的环检测；上游链必须由调用方保持无环并在处理期间稳定。
class IEffectNode : public IAudioNode
{
public:
    /// @brief 构造效果节点，并按引擎默认格式预备上游缓冲区。
    IEffectNode();

    /// @brief 析构效果节点。
    ~IEffectNode() override = default;

    /// @brief 预备音频回调复用的上游缓冲区。
    /// @param format 音频回调使用的固定格式。
    /// @param maxFrames 单个 block 允许的最大帧数。
    /// @details
    /// 零声道、零采样率或零容量会使后续处理静音返回，不沿用原准备状态。
    /// @warning 该函数会分配内存，只能在节点尚未接入运行中音频图时调用。
    void prepare(const AudioDataFormat& format, std::size_t maxFrames);

    /// @brief 从上游拉取一个 block 并应用效果。
    /// @param buffer 输出缓冲区。
    /// @details 无上游、格式不符或超出准备容量时清空输出，不调用派生效果。
    /// 上游填充前会清零输入；派生效果仍须自行写满输出，基类不预先复制或清空有效输出。
    /// @warning 音频回调热路径：每个音频 block 调用；不得引入锁、内存分配、
    /// shared_ptr 复制或阻塞操作。
    void process(AudioBuffer& buffer) override;

    /// @brief 指定输入节点。
    /// @param input 输入节点。
    /// @details
    /// 成员保留共享所有权；替换可立即析构旧节点，空指针使后续处理静音。
    /// @warning 必须在节点未被音频线程拉取时调用。
    inline void set_inputnode(std::shared_ptr<IAudioNode> input)
    {
        inputNode = std::move(input);
    }

protected:
    /// @brief 获取输入节点并共享其所有权。
    /// @return 当前输入节点。
    /// @warning 不得在音频回调调用，以免修改 shared_ptr 引用计数。
    inline std::shared_ptr<ice::IAudioNode> get_inputnode() const
    {
        return inputNode;
    }

    /// @brief 获取不增加共享所有权计数的上游观察指针。
    /// @return 当前上游节点；未设置时返回 nullptr。
    /// @warning
    /// 音频回调热路径：调用方必须保证音频设备停止前不替换或释放上游节点。
    [[nodiscard]] inline ice::IAudioNode* get_inputnode_observer() const
    {
        return inputNode.get();
    }

    /// @brief 获取已预备的输入缓冲区。
    /// @return 输入缓冲区引用。
    /// @warning 音频热路径可借用，不得扩容或保存引用供另一线程并发操作。
    /// prepare 可调整底层存储，借用引用不保证旧的样本指针仍然有效。
    inline AudioBuffer& get_inputbuffer() { return inputBuffer; }

    /// @brief 将效果应用到输入并写入输出。
    /// @param output 输出缓冲区。
    /// @param input 已由上游填充的输入缓冲区。
    /// @pre 两块缓冲格式及活动帧数一致；输出不与此基类的上游输入缓冲重叠。
    /// @details 必须覆盖完整活动输出区；空实现不会自动直通或静音。
    /// @warning 音频回调热路径：不得调整容器容量、分配内存或获取锁。
    virtual void apply_effect(AudioBuffer&       output,
                              const AudioBuffer& input) = 0;

private:
    /// @brief 上游节点。
    std::shared_ptr<ice::IAudioNode> inputNode{ nullptr };

    /// @brief 控制线程预分配、音频线程复用的上游输入缓冲区。
    AudioBuffer inputBuffer;

    /// @brief 输入缓冲区对应的固定音频格式。
    AudioDataFormat m_preparedFormat{};

    /// @brief 输入缓冲区已经预备的最大帧数。
    std::size_t m_maxPreparedFrames{ 0U };

    /// @brief 当前预备参数是否有效。
    bool m_isPrepared{ false };
};
}  // namespace ice
