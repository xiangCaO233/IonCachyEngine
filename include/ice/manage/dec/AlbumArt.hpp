#include <cstdint>
#include <cstring>

namespace ice
{
/// @brief 拥有压缩封面字节，复制时深拷贝、移动时转移所有权。
/// 图像尺寸及像素格式不由本容器解析，size 表示编码数据的字节数。
/// @warning 复制及赋值可能分配内存，不可放入音频处理回调。
class AlbumArt
{
public:
    /// @brief 独占的字节数组；外部借用者不得释放或延长其生命周期。
    uint8_t* data = nullptr;
    /// @brief 数据字节数，必须与 data 所指向的分配容量一致。
    size_t size = 0;

    /// @brief 默认构造为空封面，不分配字节数组。
    AlbumArt() = default;

    /// @brief 释放仍归当前对象所有的封面数据。
    ~AlbumArt() { delete[] data; }

    /// @brief 创建独立副本；空源保持空状态。
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
    AlbumArt& operator=(const AlbumArt& other)
    {
        // 必须在释放目标前判断自赋值，否则会同时破坏源字节。
        // 处理自我赋值
        if ( this == &other ) return *this;
        // 释放旧资源
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
    AlbumArt(AlbumArt&& other) noexcept
    {
        // "窃取" other 资源
        data = other.data;
        size = other.size;
        // 清空源的地址和长度，使其可以安全析构或重新接收封面。
        // 将 other 置为空状态，防止它在析构时释放被窃取的内存
        other.data = nullptr;
        other.size = 0;
    }

    /// @brief 先释放当前数组，再接管源数组。
    AlbumArt& operator=(AlbumArt&& other) noexcept
    {
        // 处理自我赋值
        if ( this == &other ) return *this;
        // 释放旧资源
        delete[] data;
        // 窃取 other 的资源
        data = other.data;
        size = other.size;
        // 将 other 置为空状态
        other.data = nullptr;
        other.size = 0;
        return *this;
    }

    /// @brief 只检查容器非空，不验证编码数据是否能被图像解码器识别。
    inline bool isValid() const { return data != nullptr && size > 0; }
};
}  // namespace ice
