#ifndef ICE_AUDIOPOOL_HPP
#define ICE_AUDIOPOOL_HPP

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>

#include "ice/config/config.hpp"
#include "ice/manage/AudioTrack.hpp"
#include "ice/manage/dec/IDecoderFactory.hpp"
#include "ice/thread/ThreadPool.hpp"

namespace ice
{
enum class CodecBackend {
    FFMPEG,
    COREAUDIO,
};
// 透明的哈希结构体。
// 能对 std::string, const char*, std::string_view进行哈希，
// 无需创建 std::string 对象
struct StringHash {
    // 这个标签用于开启透明性
    using is_transparent = void;
    [[nodiscard]] size_t operator()(const char* txt) const
    {
        return std::hash<std::string_view>{}(txt);
    }
    [[nodiscard]] size_t operator()(std::string_view txt) const
    {
        return std::hash<std::string_view>{}(txt);
    }
    [[nodiscard]] size_t operator()(const std::string& txt) const
    {
        return std::hash<std::string>{}(txt);
    }
};

class AudioPool
{
public:
    // 构造AudioPool
    explicit AudioPool(
        CodecBackend codec_backend = ICEConfig::default_codec_backend);
    // 析构AudioPool
    virtual ~AudioPool() = default;

    /// @brief 从缓存中移除指定音频文件。
    /// @param file UTF-8 音频文件路径。
    void invalidate(std::string_view file)
    {
        std::unique_lock<std::shared_mutex> lock(pool_mutex);
        if ( auto it = pool.find(file); it != pool.end() ) {
            pool.erase(it);
        }
    }

    // 载入文件到音频池
    template<std::convertible_to<std::string_view> StringLike>
    [[nodiscard]] std::weak_ptr<AudioTrack>
    get_or_load(ThreadPool& thread_pool, const StringLike& file,
                CachingStrategy strategy = CachingStrategy::CACHY)
    {
        std::string_view sv_name(file);
        const auto       currentSignature = read_file_signature(sv_name);
        // 使用共享锁,允许多个线程同时读取
        {
            std::shared_lock<std::shared_mutex> lock(pool_mutex);
            auto                                it = pool.find(sv_name);
            if ( it != pool.end() && it->second.track &&
                 it->second.signature == currentSignature ) {
                // 资源已缓存且文件未变化-返回结果
                return it->second.track;
            }
        }
        // 资源可能不存在,或者已过期
        // 释放读锁

        // 写锁保护
        // 使用排他锁,只允许一个线程进入写入流程
        std::unique_lock<std::shared_mutex> lock(pool_mutex);

        // 再次检查
        // 等待写锁的期间可能有另一个线程完成了加载
        auto it = pool.find(sv_name);
        if ( it != pool.end() ) {
            if ( it->second.track &&
                 it->second.signature == currentSignature ) {
                // 另一个线程提前写入完成,直接返回它的结果
                return it->second.track;
            }
            // 文件已变化或资源无效,移除旧缓存
            pool.erase(it);
        }
        // 需要加载
        // 独占写锁,创建资源
        auto new_data =
            AudioTrack::create(sv_name, thread_pool, decoder_factory, strategy);

        // C++20 map::emplace的键类型必须是key_type,所以需要构造string
        pool.emplace(std::string(sv_name),
                     CachedTrack{ new_data, currentSignature });

        return new_data;
    }

private:
    /// @brief 文件状态签名，用于判断同一路径内容是否变化。
    struct FileSignature {
        /// @brief 文件存在且状态读取成功。
        bool valid{ false };

        /// @brief 文件字节大小。
        std::uintmax_t size{ 0 };

        /// @brief 文件最后修改时间。
        std::filesystem::file_time_type write_time{};

        /// @brief 比较两个文件状态签名。
        /// @param rhs 右侧签名。
        /// @return 内容一致时返回 true。
        bool operator==(const FileSignature& rhs) const
        {
            return valid == rhs.valid && size == rhs.size &&
                   write_time == rhs.write_time;
        }
    };

    /// @brief 缓存的音轨和文件状态。
    struct CachedTrack {
        /// @brief 已解码或正在解码的音轨。
        std::shared_ptr<AudioTrack> track;

        /// @brief 载入时的文件状态签名。
        FileSignature signature;
    };

    /// @brief 将 UTF-8 路径字符串转换为文件系统路径。
    /// @param file UTF-8 路径字符串。
    /// @return 文件系统路径。
    static std::filesystem::path make_filesystem_path(std::string_view file)
    {
#ifdef __cpp_char8_t
        const auto* data = reinterpret_cast<const char8_t*>(file.data());
        return std::filesystem::path(std::u8string(data, data + file.size()));
#else
        return std::filesystem::u8path(std::string(file));
#endif
    }

    /// @brief 读取文件状态签名。
    /// @param file UTF-8 音频文件路径。
    /// @return 文件状态签名；失败时 valid 为 false。
    static FileSignature read_file_signature(std::string_view file)
    {
        std::error_code filesystemError;
        const auto      path = make_filesystem_path(file);
        if ( !std::filesystem::is_regular_file(path, filesystemError) ||
             filesystemError ) {
            return {};
        }

        FileSignature signature;
        signature.size = std::filesystem::file_size(path, filesystemError);
        if ( filesystemError ) {
            return {};
        }
        signature.write_time =
            std::filesystem::last_write_time(path, filesystemError);
        if ( filesystemError ) {
            return {};
        }
        signature.valid = true;
        return signature;
    }

    // 读写锁--可同时读,写时不可读不可写
    mutable std::shared_mutex pool_mutex;
    // 解码器工厂实现
    std::shared_ptr<IDecoderFactory> decoder_factory;
    // 音频轨道池
    std::unordered_map<std::string, CachedTrack, StringHash, std::equal_to<>>
        pool;
};
}  // namespace ice

#endif  // ICE_AUDIOPOOL_HPP
