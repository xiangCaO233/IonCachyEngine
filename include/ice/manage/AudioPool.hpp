#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>

#include "ice/config/config.hpp"
#include "ice/manage/AudioTrack.hpp"

namespace ice
{
class ThreadPool;
class IDecoderFactory;
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
    /// @brief 使用调用方提供的解码工厂，音频池共享其生命周期。
    /// @param decoder_factory 可被多个资源并发调用的工厂。
    explicit AudioPool(std::shared_ptr<IDecoderFactory> decoder_factory);
    /// @brief 释放缓存引用；调用方须先等待正在使用音频池的操作结束。
    virtual ~AudioPool() = default;

    /// @brief 同时移除指定路径的两种缓存策略及其待加载条目。
    /// @param file UTF-8 音频文件路径。
    /// @warning 低频控制侧操作，可能等待被移除的预读线程退出。
    void invalidate(std::string_view file);

    /// @brief 释放仅由音频池自身保活的缓存音轨。
    /// @return 本次从缓存中移除的音轨数量。
    /// @warning 低频回收路径；不可在音频回调或每帧更新中调用。
    [[nodiscard]] std::size_t release_unused();

    /// @brief 返回签名及策略匹配的音轨；同一加载条目共享创建结果。
    /// @param thread_pool 解码任务使用的线程池，生命周期须覆盖后台任务。
    /// @param file 非空且无内嵌零字节的 UTF-8 路径，插入时复制为拥有型键。
    /// @param strategy 缓存策略参与命中判断，非法值返回空且不改变缓存。
    /// @return 音轨弱引用，不保证后台解码完成；加载期间失效返回空。
    /// @warning 会查询文件系统；未命中可能等待同一条目的加载。
    /// 必须从低频资源加载流程调用，不能用于每帧查表或音频回调取样。
    template<std::convertible_to<std::string_view> StringLike>
    [[nodiscard]] std::weak_ptr<AudioTrack>
    get_or_load(ThreadPool& thread_pool, const StringLike& file,
                CachingStrategy strategy = ICEConfig::default_caching_strategy)
    {
        return get_or_load_impl(thread_pool, std::string_view(file), strategy);
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

    /// @brief 同一文件签名的创建槽；成员不在槽脱离索引时被修改。
    struct CachedTrack {
        /// @brief 领取槽后仅由 call_once 完成一次加载；等待发生在索引锁外。
        std::once_flag loadOnce;
        /// @brief 发布后只读的音轨。索引读取由缓存锁同步，等待者由 call_once
        /// 同步。
        std::shared_ptr<AudioTrack> track;
        /// @brief 载入时的状态签名，创建后不变。
        FileSignature signature;
    };

    /// @brief 共享槽允许加载、回收和删除索引分别持有自己的生命周期。
    using TrackSlot = std::shared_ptr<CachedTrack>;
    /// @brief 保留透明查询，使常见命中路径无需分配临时键。
    using Cache =
        std::unordered_map<std::string, TrackSlot, StringHash, std::equal_to<>>;

    /// @brief 读取文件状态签名，系统查询失败时返回无效签名。
    static FileSignature read_file_signature(std::string_view file);

    /// @brief 在索引中命中或领取待加载条目，离开索引锁后执行或等待解码。
    [[nodiscard]] std::weak_ptr<AudioTrack> get_or_load_impl(
        ThreadPool& thread_pool, std::string_view file,
        CachingStrategy strategy);

    /// @brief 仅保护缓存索引与结果发布，不覆盖探测、解码或音轨析构。
    mutable std::shared_mutex m_poolMutex;
    /// @brief 共享解码工厂，池构造之后不会更换。
    std::shared_ptr<IDecoderFactory> m_decoderFactory;
    /// @brief 完整缓存，与流式缓存按同一路径独立协调。
    Cache m_pool;
    /// @brief 流式缓存，与完整缓存共用索引锁但不共用加载条目。
    Cache m_streamingPool;
};
}  // namespace ice
