#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace ice
{
/// @brief 独占压缩封面字节，复制时深拷贝，移动时转移存储。
/// 图像编码由调用方解析，本容器只保证字节存储与长度一致。
/// @warning 复制、赋值和销毁涉及堆内存，仅用于资源加载阶段。
class AlbumArt
{
public:
    /// @brief 默认构造空封面，不分配样本存储。
    AlbumArt() = default;
    /// @brief 深拷贝字节，两个对象不共享可变存储。
    AlbumArt(const AlbumArt&) = default;
    /// @brief 复制封面内容，由容器管理旧存储及分配失败路径。
    AlbumArt& operator=(const AlbumArt&) = default;
    /// @brief 接管存储并将源恢复为空封面。
    AlbumArt(AlbumArt&& other) noexcept : m_data(std::move(other.m_data))
    {
        // 明确保持历史移动后空状态，不向调用方暴露容器未指定的源内容。
        other.m_data.clear();
    }
    /// @brief 接管源字节并释放旧内容，自移动保留原封面。
    /// @warning 不分配但可能释放旧数组，只能用于非实时资源路径。
    AlbumArt& operator=(AlbumArt&& other) noexcept
    {
        // 自移动不能清除仍作为源的同一容器。
        if ( this == &other ) return *this;
        m_data = std::move(other.m_data);
        other.m_data.clear();
        return *this;
    }
    /// @brief 用独立副本替换封面，允许输入借用当前对象的一段字节。
    /// @param bytes 调用期间有效的只读编码数据，空视图清空封面。
    /// @return 尺寸超过容器上限时为 false，原内容保持不变。
    /// @warning 低频分配入口；实际分配失败仍遵循标准容器异常机制。
    bool assign(std::span<const uint8_t> bytes)
    {
        if ( bytes.size() > m_data.max_size() ) return false;
        // 先完成副本再交换，借用自身的输入不会因旧存储回收而悬空。
        // 候选分配失败不会先销毁目标；空输入也以同一替换契约处理。
        std::vector<uint8_t> replacement(bytes.begin(), bytes.end());
        m_data.swap(replacement);
        return true;
    }
    /// @brief 返回只读借用地址；修改、移动或销毁对象可使借用失效。
    const uint8_t* data() const noexcept { return m_data.data(); }
    /// @brief 返回实际拥有的编码字节数，不由外部独立修改。
    size_t size() const noexcept { return m_data.size(); }
    /// @brief 仅判断是否包含字节，不证明图像编码可被解码。
    bool isValid() const noexcept { return !m_data.empty(); }

private:
    /// @brief 唯一拥有的编码数据，地址与长度从同一容器派生。
    std::vector<uint8_t> m_data;
};
}  // namespace ice
