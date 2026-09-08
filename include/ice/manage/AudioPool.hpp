#ifndef ICE_AUDIOPOOL_HPP
#define ICE_AUDIOPOOL_HPP

#include <algorithm>
#include <cstddef>
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
/// @brief 音频池解码器选择；COREAUDIO 当前仅为保留枚举，未建立工厂。
enum class CodecBackend {
    FFMPEG,
    COREAUDIO,
};
/// @brief 为拥有型路径键和借用型查询提供一致的透明哈希。
struct StringHash {
    /// @brief 允许 unordered_map 查找借用字符串，避免为查缓存构造路径键。
    using is_transparent = void;
    /// @brief 对零结尾字符串按内容计算哈希，调用方保证非空有效指针。
    [[nodiscard]] size_t operator()(const char* txt) const
    {
        return std::hash<std::string_view>{}(txt);
    }
    /// @brief 对限定长度的借用字符串计算哈希，不要求零结尾。
    [[nodiscard]] size_t operator()(std::string_view txt) const
    {
        return std::hash<std::string_view>{}(txt);
    }
    /// @brief 为存储键提供与等值 string_view 一致的哈希结果。
    [[nodiscard]] size_t operator()(const std::string& txt) const
    {
        return std::hash<std::string>{}(txt);
    }
};

/// @brief 按路径及文件状态缓存共享音轨，加载和资源回收均属于非实时工作。
/// 相对路径、大小写或符号链接不会归一化，调用方须统一路径表示以共享缓存。
class AudioPool
{
public:
    /// @brief 选择解码工厂，当前仅 FFMPEG 建立可用实现。
    explicit AudioPool(
        CodecBackend codec_backend = ICEConfig::default_codec_backend);
    /// @brief 释放缓存持有的音轨引用；外部强引用可继续保活已取出的音轨。
    virtual ~AudioPool() = default;

    /// @brief 从缓存中移除指定音频文件。
    /// @param file UTF-8 音频文件路径。
    /// @warning 低频控制侧独占锁操作，可能析构完整音轨，不可在音频回调调用。
    void invalidate(std::string_view file)
    {
        // 不读取文件签名，显式失效适用于同大小、同时间戳覆盖后强制重载。
        std::unique_lock<std::shared_mutex> lock(pool_mutex);
        // 两种策略按同一文件一起失效，已发布的播放引用继续保活旧资源。
        for ( auto* cache : { &pool, &m_streamingPool } ) {
            if ( auto it = cache->find(file); it != cache->end() )
                cache->erase(it);
        }
    }

    /// @brief 释放仅由音频池自身保活的缓存音轨。
    /// @return 本次从缓存中移除的音轨数量。
    /// @warning 低频资源回收路径：会独占缓存锁并可能析构完整 PCM，禁止在
    /// 音频回调或每帧更新中调用。
    [[nodiscard]] std::size_t release_unused()
    {
        std::unique_lock<std::shared_mutex> lock(pool_mutex);
        std::size_t                         released = 0;
        for ( auto* cache : { &pool, &m_streamingPool } ) {
            const auto previousSize = cache->size();
            std::erase_if(*cache, [](const auto& entry) {
                // 只有缓存保活时才能停止预读或回收 PCM，不撤销正在播放的引用。
                return !entry.second.track ||
                       entry.second.track.use_count() == 1;
            });
            released += previousSize - cache->size();
        }
        return released;
    }

    /// @brief 返回匹配文件状态的音轨，缓存未命中时在写锁下创建音轨。
    /// @param thread_pool 音轨解码任务使用的线程池，生命周期须覆盖后台任务。
    /// @param file UTF-8 路径借用值，插入缓存时复制为拥有型键。
    /// @param strategy 缓存策略参与命中判断，不同策略不会误用旧音轨。
    /// @return 音轨弱引用，不保证后台解码完成，也不替调用方持有播放期所有权。
    /// @warning 每次调用均查询文件系统，未命中可分配、等待锁或创建解码任务。
    /// 必须从低频资源加载流程调用，不能用于每帧查表或音频回调取样。
    template<std::convertible_to<std::string_view> StringLike>
    [[nodiscard]] std::weak_ptr<AudioTrack>
    get_or_load(ThreadPool& thread_pool, const StringLike& file,
                CachingStrategy strategy = ICEConfig::default_caching_strategy)
    {
        // 播放与分析可以同时使用同一路径，不因另一策略加载而逐出已有资源。
        auto& cache =
            strategy == CachingStrategy::STREAMING ? m_streamingPool : pool;
        std::string_view sv_name(file);
        const auto       currentSignature = read_file_signature(sv_name);
        // 签名读取在缓存锁外，减少持锁 IO；文件并发写入时不构成内容快照。
        // 使用共享锁,允许多个线程同时读取
        {
            std::shared_lock<std::shared_mutex> lock(pool_mutex);
            auto                                it = cache.find(sv_name);
            if ( it != cache.end() && it->second.track &&
                 it->second.signature == currentSignature &&
                 it->second.track->cachingStrategy() == strategy ) {
                // 资源已缓存且文件未变化-返回结果
                return it->second.track;
            }
        }
        // 从读锁升级为写锁之前存在竞争窗口，下面必须再次检查缓存。

        // 同路径并发未命中只创建一次资源；创建期间也会阻塞其他路径的写入。
        std::unique_lock<std::shared_mutex> lock(pool_mutex);

        // 再次检查
        // 等待写锁的期间可能有另一个线程完成了加载
        auto it = cache.find(sv_name);
        if ( it != cache.end() ) {
            if ( it->second.track && it->second.signature == currentSignature &&
                 it->second.track->cachingStrategy() == strategy ) {
                // 另一个线程提前写入完成,直接返回它的结果
                return it->second.track;
            }
            // 文件已变化或资源无效,移除旧缓存
            cache.erase(it);
        }
        // 建立音轨不等于全量 PCM 已就绪，缓存可持有仍在后台解码的对象。
        auto new_data =
            AudioTrack::create(sv_name, thread_pool, decoder_factory, strategy);

        // 缓存键必须拥有路径文本，不能把调用方临时 string_view 存入 map。
        cache.emplace(std::string(sv_name),
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
        /// @return 状态字段一致时返回 true，不保证文件内容字节完全相同。
        bool operator==(const FileSignature& rhs) const
        {
            // 大小和时间不变的覆盖写无法被发现；两个无效签名也会比较相等。
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
        // 显式指定 UTF-8 编码，不让 Windows 窄字符路径按本地代码页解释。
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
            // 缺失、目录或权限错误均返回无效签名，不缓存半次状态读取结果。
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
        // 三次文件系统查询并非原子操作，加载后文件仍可能变化，下次访问再复查。
        return signature;
    }

    /// @brief 保护缓存条目及所有权更新；读操作可并发，加载和回收独占。
    mutable std::shared_mutex pool_mutex;
    /// @brief 共享解码工厂，创建音轨时传递给其后台加载流程。
    std::shared_ptr<IDecoderFactory> decoder_factory;
    /// @brief 拥有路径键与音轨强引用，文件状态仅用作低成本失效判断。
    std::unordered_map<std::string, CachedTrack, StringHash, std::equal_to<>>
        pool;
    /// @brief 流式音轨独立保活，键与失效签名规则和完整缓存一致。
    /// 两个映射共用 pool_mutex，切换策略不会破坏另一策略的缓存身份。
    /// invalidate 必须同时处理两张表，避免文件覆盖后继续读取旧流。
    /// release_unused
    /// 按各轨道的外部所有权回收，不依赖另外一种策略是否仍在播放。
    /// 流式对象析构可能等待后台块读取，因此回收仍限于低频控制侧。
    std::unordered_map<std::string, CachedTrack, StringHash, std::equal_to<>>
        m_streamingPool;
};
}  // namespace ice

#endif  // ICE_AUDIOPOOL_HPP
