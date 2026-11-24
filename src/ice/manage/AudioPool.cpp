#include <ice/manage/AudioPool.hpp>

#include "ice/manage/dec/ffmpeg/FFmpegDecoderFactory.hpp"

#include <array>
#include <filesystem>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace ice
{
/// @brief 按配置建立共享解码工厂，尚不打开媒体或启动解码。
/// @param codec_backend 当前实际实现仅支持 FFMPEG。
/// @warning 构造分配共享工厂，须位于非实时资源管理流程。
AudioPool::AudioPool(CodecBackend codec_backend)
{
    // 工厂由多个新音轨共享；具体媒体状态仍归各解码实例独占。
    switch ( codec_backend ) {
    case CodecBackend::FFMPEG: {
        // 工厂不保留当前媒体句柄，每个音轨建立各自的解码上下文。
        m_decoderFactory = std::make_shared<FFmpegDecoderFactory>();
        break;
    }
    case CodecBackend::COREAUDIO: {
        // 保留后端枚举但不创建实例，此分支留下空工厂，不能用于成功加载。
        // 调用方须选择已有实现；本构造没有返回错误的接口。
        // 不隐式回退到 FFmpeg，保持调用方的后端选择语义。
        break;
    }
    }
}
/// @brief 共享调用方的解码工厂，允许多个独立路径并行调用它。
/// @param decoder_factory 池构造之后不会被替换。退出前应停止所有调用者。
/// 空工厂保留无可用后端的行为，由 AudioTrack::create 返回空结果。
AudioPool::AudioPool(std::shared_ptr<IDecoderFactory> decoder_factory)
    : m_decoderFactory(std::move(decoder_factory))
{
}

/// @brief 断开指定路径与两种载入策略的当前条目。
/// @param file 只在查找期间借用文本，无需查询磁盘状态。
/// @details 已发布给播放节点的强引用不受影响；正在建立的槽只失去发布资格。
/// @warning 调用线程可能等待退役音轨的析构，不应是音频回调线程。
void AudioPool::invalidate(std::string_view file)
{
    // 在锁外释放移除的所有权。流式音轨的析构可能等待后端完成一次读取。
    // 每套缓存最多摘除一个键，结果数量固定。准备外部存储后用同一临界区
    // 摘除两种策略，调用者不会在两次失效操作之间观察到内容不一致。
    std::array<TrackSlot, 2> retired;
    {
        std::unique_lock lock(m_poolMutex);
        std::size_t      index = 0;
        for ( auto* cache : { &m_pool, &m_streamingPool } ) {
            if ( auto it = cache->find(file); it != cache->end() ) {
                // 待加载槽也从索引脱离，旧任务完成后不能重新发布这个路径。
                retired[index++] = std::move(it->second);
                cache->erase(it);
            }
        }
    }
}

/// @brief 批量移除只由缓存保活的条目，并在解锁后回收媒体资源。
/// @return 此次摘除的条目数。外部弱引用可以在取样引用计数前后正常提升。
/// @note 释放索引的强引用不会停止仍被其它方引用的音轨。
std::size_t AudioPool::release_unused()
{
    std::vector<TrackSlot> retired;
    {
        std::unique_lock lock(m_poolMutex);
        for ( auto* cache : { &m_pool, &m_streamingPool } ) {
            for ( auto it = cache->begin(); it != cache->end(); ) {
                const auto& slot = it->second;
                // 调用方可能仍在等待或执行初始化；空槽只有失去所有调用方后才可回收。
                // 发布的轨道则只看外部强引用，撤销索引不会打断已持有它的播放实例。
                // 无音轨时用槽所有者计数。异常退出后无人持有的空槽也能回收，
                // 本函数不需要知道工厂是否未启动、重试或失败退出。
                const bool unused = slot->track ? slot->track.use_count() == 1
                                                : slot.use_count() == 1;
                if ( !unused ) {
                    ++it;
                    continue;
                }
                // 先确保退役列表持有条目；分配失败时原索引仍能保活该资源。
                // 列表销毁发生在解锁后，回收大块 PCM
                // 或等待预读都不阻塞其他路径。
                retired.push_back(slot);
                it = cache->erase(it);
            }
        }
    }
    // 返回值只统计本轮在锁内选中的项目，无需对释放期间的新查询再次加锁。
    return retired.size();
}

/// @brief 根据路径、文件状态和策略借用音轨，避免在索引锁内执行任何解码。
/// @param thread_pool 完整缓存后台解码所需的池，由取得播放资源的调用方维持。
/// @param file UTF-8 路径，在完整调用期间保持有效。
/// @param strategy 完整缓存和流式缓存可使用同一路径维持不同的解码状态。
/// @return 已发布的音轨；显式失效或新签名取代本次结果时返回空。
/// @details 每个槽代表一个具体签名。槽指针身份比路径或签名本身更严格，
/// 因此外部同大小、同时间戳覆盖后也能通过 invalidate 更新加载代次。
/// @warning 访问文件并可能等待同一槽，仅限资源加载路径。
std::weak_ptr<AudioTrack> AudioPool::get_or_load_impl(ThreadPool& thread_pool,
                                                      std::string_view file,
                                                      CachingStrategy  strategy)
{
    // 拒绝无效策略和 C 文件接口会截断的路径；不可隐式逐出已有的有效条目。
    if ( (strategy != CachingStrategy::CACHY &&
          strategy != CachingStrategy::STREAMING) ||
         file.empty() || file.find('\0') != std::string_view::npos )
        return {};

    auto& cache =
        strategy == CachingStrategy::STREAMING ? m_streamingPool : m_pool;
    // 文件状态读取与解码均在索引锁外；签名仅是失效线索，不保证文件内容快照。
    // 如果文件名只有视图，透明查找可直接命中；只在真正注册新路径时复制键。
    const auto signature = read_file_signature(file);
    TrackSlot  slot;
    {
        std::shared_lock lock(m_poolMutex);
        const auto       it = cache.find(file);
        if ( it != cache.end() && it->second->signature == signature ) {
            if ( it->second->track ) return it->second->track;
            // 条目加载正在进行，保留独立所有权后在槽上等待。
            slot = it->second;
        }
    }

    // 写锁仅用于安装或替换条目。重新查找隔离从读锁过渡到写锁时的竞争。
    // 被替换的音轨必须一直保活到锁外，避免在覆盖索引时触发析构。
    // 替换槽和取得待加载槽都不会等待其执行者，因此另一方可以继续发布新版本。
    TrackSlot replaced;
    if ( !slot ) {
        std::unique_lock lock(m_poolMutex);
        const auto       it = cache.find(file);
        if ( it != cache.end() && it->second->signature == signature ) {
            if ( it->second->track ) return it->second->track;
            slot = it->second;
        } else {
            slot            = std::make_shared<CachedTrack>();
            slot->signature = signature;
            if ( it == cache.end() ) {
                cache.emplace(std::string(file), slot);
            } else {
                replaced = std::exchange(it->second, slot);
            }
        }
    }

    std::call_once(slot->loadOnce, [&] {
        // 只有领取同一加载槽的线程在这里串行；另一个路径或策略可独立加载。
        // call_once 选择一个调用者执行创建，其余调用者同步观察完成结果。
        // 工厂若沿用历史异常行为，异常仍向上传递；call_once 允许后续调用重试。
        // 流式首块也在锁外解码；完全缓存仍按原有规则异步解码后续 PCM。
        auto track =
            AudioTrack::create(file, thread_pool, m_decoderFactory, strategy);
        std::unique_lock lock(m_poolMutex);
        const auto       it = cache.find(file);
        // 同路径可能已明确失效或切换文件签名。旧工作不可覆盖新条目。
        // 由于 track 在锁之前构造，孤立的加载结果也只会在解锁后销毁。
        if ( it == cache.end() || it->second != slot ) return;
        if ( track ) {
            slot->track = std::move(track);
        } else {
            // 本次等待者获得同一个失败结果；之后的新调用可重新加载。
            // 当前线程仍持有槽，清除索引不会在这个临界区销毁条目。
            cache.erase(it);
        }
    });
    // call_once 同步结果发布。缓存回收只摘除条目，不再写入已发布的音轨成员。
    return slot->track;
}

/// @brief 读取 UTF-8 文件签名，失败时统一返回无效值。
/// @param file 已验证非空且不含零字节。
/// @return 普通文件的大小与写入时间；缺失、权限错误或目录返回无效签名。
AudioPool::FileSignature AudioPool::read_file_signature(std::string_view file)
{
#ifdef __cpp_char8_t
    // 显式指定 UTF-8，避免 Windows 依据当前代码页解释窄字符路径。
    const auto* data = reinterpret_cast<const char8_t*>(file.data());
    const std::filesystem::path path(std::u8string(data, data + file.size()));
#else
    const auto path = std::filesystem::u8path(std::string(file));
#endif

    // 仅普通文件具备可供复查的大小与写入时间。
    // 路径可能在三次状态读取之间被重命名，不保留只读成功一部分的签名。
    std::error_code error;
    if ( !std::filesystem::is_regular_file(path, error) || error ) return {};

    FileSignature signature;
    signature.size = std::filesystem::file_size(path, error);
    if ( error ) return {};
    signature.write_time = std::filesystem::last_write_time(path, error);
    if ( error ) return {};
    // 三次查询不以事务方式读取；下次访问仍检查此签名是否失效。
    signature.valid = true;
    return signature;
}
}  // namespace ice
