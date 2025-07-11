#ifndef ICE_AUDIOPOOL_HPP
#define ICE_AUDIOPOOL_HPP

#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>

#include "ice/manage/AudioTrack.hpp"

namespace ice {
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
   private:
    // 读写锁--可同时读,写时不可读不可写
    mutable std::shared_mutex pool_mutex;

   public:
    // 构造AudioPool
    AudioPool();
    // 析构AudioPool
    virtual ~AudioPool() = default;

    // 载入文件到音频池
    template <std::convertible_to<std::string_view> StringLike>
    [[nodiscard]] std::shared_ptr<AudioTrack> get_or_load(
        const StringLike& file,
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
                // 资源已过期，需要移除
                pool.erase(it);
            }
        }
        // ---- 确认需要加载,执行昂贵操作 ----
        // 此时我们独占了写锁,可以安全地创建和插入新资源
        // 模拟昂贵的加载过程
        auto new_data = AudioTrack::create(sv_name, strategy);

        // C++20 map::emplace的键类型必须是key_type,所以需要构造string
        pool.emplace(std::string(sv_name), new_data);

        return new_data;
    }

   protected:
    std::unordered_map<std::string, std::weak_ptr<AudioTrack>, StringHash,
                       std::equal_to<>>
        pool;
};
}  // namespace ice

#endif  // ICE_AUDIOPOOL_HPP
