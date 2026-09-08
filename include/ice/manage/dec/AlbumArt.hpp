#include <cstdint>
#include <cstring>

namespace ice
{
/// @brief 拥有压缩封面字节，复制时深拷贝、移动时转移所有权。
/// 图像尺寸及像素格式不由本容器解析，size 表示编码数据的字节数。
/// @warning 复制及赋值可能分配内存，不可放入音频处理回调。
/// 公开字段允许外部破坏所有权不变量；本类不校验数组来源、实际容量或指针别名。
/// 各对象必须独占可由 delete[] 释放的数组，不能把同一地址交给两个对象。
class AlbumArt
{
public:
    /// @brief 独占的字节数组；外部借用者不得释放或延长其生命周期。
    /// 直接覆盖非空地址不会自动回收旧数组，写入者必须先处理原所有权。
    uint8_t* data = nullptr;
    /// @brief 数据字节数，必须与 data 所指向的分配容量一致。
    size_t size = 0;

    /// @brief 默认构造为空封面，不分配字节数组。
    AlbumArt() = default;

    /// @brief 释放仍归当前对象所有的封面数据。
    /// @warning 销毁会回收数组，所有借用指针随即失效，不应在实时回调触发。
    ~AlbumArt() { delete[] data; }

    /// @brief 创建独立副本；空源保持空状态。
    /// @param other 复制期间稳定的源对象，非空数据必须至少有 size 字节可读。
    AlbumArt(const AlbumArt& other)
    {
        // 空源不读取地址，也不为零长度封面分配数组。
        if ( other.data && other.size > 0 ) {
            size = other.size;
            data = new uint8_t[size];
            memcpy(data, other.data, size);
        }
    }

    /// @brief 替换封面内容，源对象与目标不共享字节数组。
    /// @warning 旧内容先释放；此历史实现不提供分配失败时的强保证。
    /// @param other 与目标不共享底层数组的源对象，自赋值除外。
    /// @return 当前对象引用。
    AlbumArt& operator=(const AlbumArt& other)
    {
        // 必须在释放目标前判断自赋值，否则会同时破坏源字节。
        if ( this == &other ) return *this;
        // 先丢弃旧所有权；后续分配失败时 data 为空，但 size
        // 可能已更新为源长度。
        delete[] data;
        data = nullptr;
        size = 0;
        if ( other.data && other.size > 0 ) {
            size = other.size;
            data = new uint8_t[size];
            memcpy(data, other.data, size);
        }
        return *this;
    }

    /// @brief 接管源数组，不重新分配或复制封面字节。
    /// @param other
    /// 被置为空状态的源，外部借用地址仍指向现在由目标管理的同一数组。
    AlbumArt(AlbumArt&& other) noexcept
    {
        // 原地址保持不变，移动不会校验或修复源对象已有的不一致字段。
        data = other.data;
        size = other.size;
        // 清空源的地址和长度，使其可以安全析构或重新接收封面。
        other.data = nullptr;
        other.size = 0;
    }

    /// @brief 先释放当前数组，再接管源数组。
    /// @param other 被转移所有权的源，自移动时保持原对象不变。
    /// @return 当前对象引用。
    /// @warning 虽然不分配且声明
    /// noexcept，仍会释放目标原数组，不是无堆操作接口。
    AlbumArt& operator=(AlbumArt&& other) noexcept
    {
        // 必须在释放前识别自移动，避免接管刚被自身释放的地址。
        if ( this == &other ) return *this;
        // 目标旧视图在此失效；源数组的借用视图则随所有权转移继续指向原地址。
        delete[] data;
        data = other.data;
        size = other.size;
        // 取消源的释放责任，保证两个对象析构时只回收这块数组一次。
        other.data = nullptr;
        other.size = 0;
        return *this;
    }

    /// @brief 只检查容器非空，不验证编码数据是否能被图像解码器识别。
    /// @return 地址非空且长度非零；不证明地址可读、长度可信或文件格式有效。
    inline bool isValid() const { return data != nullptr && size > 0; }
};
}  // namespace ice
