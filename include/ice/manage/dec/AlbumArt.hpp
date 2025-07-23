#include <cstdint>
#include <cstring>

namespace ice {
// 管理专辑封面数据的内存
class AlbumArt {
   public:
    // 原始图像数据指针
    uint8_t* data = nullptr;
    // 图像数据大小-字节
    size_t size = 0;

    AlbumArt() = default;

    ~AlbumArt() { delete[] data; }

    // 拷贝构造函数 (深拷贝)
    AlbumArt(const AlbumArt& other) {
        if (other.data && other.size > 0) {
            size = other.size;
            data = new uint8_t[size];
            memcpy(data, other.data, size);
        }
    }

    // 拷贝赋值运算符
    AlbumArt& operator=(const AlbumArt& other) {
        // 处理自我赋值
        if (this == &other) return *this;
        // 释放旧资源
        delete[] data;
        data = nullptr;
        size = 0;
        if (other.data && other.size > 0) {
            size = other.size;
            data = new uint8_t[size];
            memcpy(data, other.data, size);
        }
        return *this;
    }

    // 移动构造函数 -- 转移所有权
    AlbumArt(AlbumArt&& other) noexcept {
        // "窃取" other 资源
        data = other.data;
        size = other.size;
        // 将 other 置为空状态，防止它在析构时释放被窃取的内存
        other.data = nullptr;
        other.size = 0;
    }

    // 移动赋值运算符
    AlbumArt& operator=(AlbumArt&& other) noexcept {
        // 处理自我赋值
        if (this == &other) return *this;
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

    // 检查是否有数据
    inline bool isValid() const { return data != nullptr && size > 0; }
};
}  // namespace ice
