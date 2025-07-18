#ifndef ICE_AUDIOPOOL_HPP
#define ICE_AUDIOPOOL_HPP

#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>

#include "ice/config/config.hpp"
#include "ice/manage/AudioTrack.hpp"
#include "ice/manage/dec/IDecoderFactory.hpp"
#include "ice/thread/ThreadPool.hpp"

namespace ice {
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
    [[nodiscard]] size_t operator()(const char* txt) const {
        return std::hash<std::string_view>{}(txt);
    }
    [[nodiscard]] size_t operator()(std::string_view txt) const {
        return std::hash<std::string_view>{}(txt);
    }
    [[nodiscard]] size_t operator()(const std::string& txt) const {
        return std::hash<std::string>{}(txt);
    }
};

class AudioPool {
   public:
    // 构造AudioPool
    explicit AudioPool(
        CodecBackend codec_backend = ICEConfig::default_codec_backend);
    // 析构AudioPool
    virtual ~AudioPool() = default;

    // 载入文件到音频池
    template <std::convertible_to<std::string_view> StringLike>
    [[nodiscard]] std::shared_ptr<AudioTrack> get_or_load(
        ThreadPool& thread_pool, const StringLike& file,
        CachingStrategy strategy = CachingStrategy::CACHY) {
        std::string_view sv_name(file);
        // 使用共享锁,允许多个线程同时读取
        {
            std::shared_lock<std::shared_mutex> lock(pool_mutex);
            auto it = pool.find(sv_name);
            if (it != pool.end()) {
                if (auto sp = it->second.lock()) {
                    // 资源已缓存且有效-返回结果
                    return sp;
                }
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
        if (it != pool.end()) {
            if (auto sp = it->second.lock()) {
                // 另一个线程提前写入完成,直接返回它的节果
                return sp;
            } else {
                // 资源过期,移除
                pool.erase(it);
            }
        }
        // 需要加载
        // 独占写锁,创建资源
        auto new_data =
            AudioTrack::create(sv_name, thread_pool, decoder_factory, strategy);

        // C++20 map::emplace的键类型必须是key_type,所以需要构造string
        pool.emplace(std::string(sv_name), new_data);

        return new_data;
    }

   private:
    // 读写锁--可同时读,写时不可读不可写
    mutable std::shared_mutex pool_mutex;
    // 解码器工厂实现
    std::shared_ptr<IDecoderFactory> decoder_factory;
    // 音频轨道池
    std::unordered_map<std::string, std::weak_ptr<AudioTrack>, StringHash,
                       std::equal_to<>>
        pool;
};
}  // namespace ice

#endif  // ICE_AUDIOPOOL_HPP
