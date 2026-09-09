#include <ice/manage/dec/CachyDecoder.hpp>

#include <ice/manage/AudioFormat.hpp>
#include <ice/manage/dec/IDecoderFactory.hpp>
#include <ice/manage/dec/IDecoderInstance.hpp>
#include <ice/thread/ThreadPool.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <future>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ice
{
/// @brief 接管任务句柄，不在构造阶段等待整文件解码。
CachyDecoder::CachyDecoder(ConstructionKey,
                           std::future<DecodedData> future_data)
    : m_futureData(std::move(future_data))
{
}

/// @brief 构建后台整文件解码任务并返回缓存策略。
/// @param path 提交时复制的非空路径，不允许嵌入空字符。
/// @param target_format 交给实例的输出格式请求，不是缓存格式已满足的证明。
/// @param thread_pool 负责最终执行任务的外部线程池。
/// @param factory 在后台创建解码实例的工厂，空值产生空缓存。
/// @return 路径、目标格式无效或提交被拒绝时返回空指针，媒体结果仍异步确定。
/// @warning 文件访问和内存增长发生在工作线程，首次消费仍可能等待。
std::unique_ptr<CachyDecoder> CachyDecoder::create(
    std::string_view path, const ice::AudioDataFormat& target_format,
    ThreadPool& thread_pool, std::shared_ptr<IDecoderFactory> factory)
{
    // 后端可能使用 C 字符串打开文件，嵌入空字符不能被悄悄解释为路径前缀。
    // 在复制路径和创建任务前拒绝，避免无效输入占用后台队列或触发工厂访问。
    if ( path.empty() || path.find('\0') != std::string_view::npos )
        return nullptr;
    // 零格式无需访问媒体即可判定失败，不能先发布策略再等待后台产生空缓存。
    // 任务按值捕获已验证格式，后续不再依赖调用方配置的生命周期或修改。
    if ( target_format.channels == 0 || target_format.samplerate == 0 )
        return nullptr;
    // 路径复制到任务内，避免调用方 string_view 在异步执行前失效。
    // 工厂按共享所有权捕获，覆盖整个后台解码期间的生命周期。
    // 任务不捕获解码器自身；销毁策略对象不会向后台任务发送取消请求。
    auto decode_task = [target_format,
                        factory,
                        path_str = std::string(path)]() -> DecodedData {
        // 已知加载失败直接发布空结果，不通过 packaged_task 再次传递异常。
        if ( !factory ) return {};
        auto worker = factory->create_instance(path_str, target_format);
        if ( !worker ) {
            return {};
        }

        // 使用实例报告的输出格式分配平面缓冲，不能只按目标声道数猜测。
        // 缓存只做复制，不能修正采样率；不匹配时拒绝以免改变实际播放速度。
        auto format = worker->get_source_format();
        // 无效输出格式不能建立可供逐声道拷贝的缓存，拒绝后端半成品。
        if ( format.channels == 0 ||
             format.samplerate != target_format.samplerate )
            return {};
        // 仅保留既有单声道复制扩展，其他声道变换需要后端真正完成混音。
        if ( format.channels != target_format.channels && format.channels != 1 )
            return {};

        // 容器时长只是元信息，不能据此在读到 PCM 前申请整段缓存。
        // 各声道随实际追加按 vector 增长策略扩容，错误估计不会直接耗尽内存。
        // 整文件模式的内存仍与真实帧数成正比，大文件应由上层选择流式策略。
        std::vector<std::vector<float>> pcm(format.channels);

        // 每个通道拥有独立连续数组；read 需要的是指针表而非交错 PCM。
        // 临时块仅用于搬运，最终缓存拥有独立存储，不借用解码器内部样本。
        const size_t                    CHUNK_SIZE = 2048;
        std::vector<float*>             chunk_ptrs(format.channels);
        std::vector<std::vector<float>> chunk_buffer(format.channels);
        for ( auto& c : chunk_buffer ) {
            c.resize(CHUNK_SIZE);
        }
        for ( uint16_t i = 0; i < format.channels; ++i ) {
            chunk_ptrs[i] = chunk_buffer[i].data();
        }

        // 创建流程强制 seek(0)，即使实例从起点开始也要求其支持此定位操作。
        if ( !worker->seek(0) ) {
            // 定位失败尚未发布 PCM，保持与实例创建失败一致的空结果语义。
            return {};
        }

        // 只追加实际读出的部分，最后一块不足 CHUNK_SIZE 时不补齐缓存。
        // read 返回零就结束，没有独立错误码区分正常 EOF 与解码失败。
        // 在构造迭代器区间前复查后端返回值，不能信任越过请求容量的帧数。
        size_t frames_read = 0;
        while ( (frames_read = worker->read(chunk_ptrs.data(), CHUNK_SIZE)) >
                0 ) {
            // 后端违约时丢弃整次缓存，避免从临时块外读取或发布不完整声道。
            if ( frames_read > CHUNK_SIZE ) return {};
            for ( uint16_t ch = 0; ch < format.channels; ++ch ) {
                pcm[ch].insert(pcm[ch].end(),
                               chunk_buffer[ch].begin(),
                               chunk_buffer[ch].begin() + frames_read);
            }
        }

        // 单声道复制到所有目标声道，不做多声道矩阵混音。
        // 复制发生在发布缓存前，消费者不会看到扩展到一半的声道布局。
        if ( format.channels == 1 && target_format.channels > 1 &&
             pcm.size() == 1 ) {
            // 先完成外层扩容再访问首声道，避免借用 vector 元素跨越其搬迁。
            // 首声道始终保留原始 PCM，无需额外复制整条音轨作为临时源。
            pcm.resize(target_format.channels);
            for ( uint16_t ch = 1; ch < target_format.channels; ++ch ) {
                pcm[ch] = pcm[0];
            }
            format.channels = target_format.channels;
        }

        // 连同格式一次性移交最终存储，不让消费侧观察仍在增长的中间缓存。
        return { format, std::move(pcm) };
    };

    // 使用调用方线程池；返回后不能假设后台任务已完成。
    auto future_result = thread_pool.enqueue(decode_task);
    // 没有共享状态表示任务未被接收，不能包装成等待后才发现为空的缓存策略。
    // 返回空值使上层音轨工厂沿既有失败分支退出，避免缓存无法执行的音轨。
    if ( !future_result.valid() ) return nullptr;

    // 返回对象保存 future，结果只能在 get_data 中被一次性提取。
    // 已入队之后若策略对象构造失败，后台任务也不会因此自动撤回。
    // 凭据由本工厂生成，公开构造只为标准库分配入口开放，不允许外部绕过工厂。
    return std::make_unique<CachyDecoder>(ConstructionKey{},
                                          std::move(future_result));
}

/// @brief 首次取得任务结果，后续访问复用缓存。
/// @warning call_once 与 future.get 均可能等待，只可在准备阶段首次调用。
/// 当前历史错误处理将 std::exception 折叠为空缓存，调用方无法据此区分具体错误。
const CachyDecoder::DecodedData& CachyDecoder::get_data() const
{
    std::call_once(m_dataReadyFlag, [this]() {
        // 提交被拒绝时没有共享状态，禁止调用 get 制造 future_error。
        if ( !m_futureData.valid() ) {
            m_dataCache = DecodedData{};
            return;
        }
        try {
            // call_once 确保仅一个线程消费
            // future，其余首次读取者等待同一结果发布。
            m_dataCache = m_futureData.get();
        } catch ( const std::exception& e ) {
            // 固定为空结果后不重新读取 future，也不会自动重试文件。
            // 异常未逃出初始化函数时 once_flag
            // 视作完成，后续调用不会再次尝试任务。
            m_dataCache = DecodedData{};
        }
    });
    return *m_dataCache;
}

/// @brief 从不可变缓存复制指定帧区间。
/// @param buffer 调用方提供的非重叠可写声道缓冲，不由解码器分配。
/// @param num_channels 目标声道数，超过缓存时仅单声道情形会复制扩展。
/// @param start_frame 缓存中的每声道起始帧。
/// @param frame_count 请求容量，末尾不足时仅写可用前缀。
/// @return 可用切片长度；未写入的目标尾部由调用方处理。
/// @warning 音频读取前应预先触发 get_data；此函数本身不保证首次访问无等待。
size_t CachyDecoder::decode(float** buffer, uint16_t num_channels,
                            size_t start_frame, size_t frame_count)
{
    // 空请求不需要缓存内容，必须在可能等待后台任务的 get_data 之前返回。
    if ( !buffer || num_channels == 0 || frame_count == 0 ) return 0;
    // 在缓存等待和任何复制之前验证输出表，错误请求不留下部分声道结果。
    for ( uint16_t channel = 0; channel < num_channels; ++channel )
        if ( !buffer[channel] ) return 0;

    // 正常播放前应已提取结果；未预备的读取会在这里承受整文件解码延迟。
    const auto& data = get_data();

    // 空缓存可能是零长度媒体，也可能是后台失败；此接口不区分二者。
    if ( data.pcm_data.empty() ) return 0;

    // 各声道等长是构建缓存时维持的不变量，此处只用首声道计算统一切片范围。
    const size_t total_frames = data.pcm_data[0].size();
    if ( start_frame >= total_frames ) return 0;

    // 先验证起点再做减法，避免起点越界时无符号剩余长度下溢。
    const size_t frames_available = total_frames - start_frame;
    const auto   frames_to_copy   = std::min(frame_count, frames_available);
    if ( frames_to_copy == 0 ) {
        return 0;
    }
    // 单声道的按需复制兼容未在创建阶段扩展声道的缓存。
    if ( data.pcm_data.size() == 1 && num_channels > 1 ) {
        const float* src = data.pcm_data[0].data() + start_frame;
        for ( uint16_t ch = 0; ch < num_channels; ++ch ) {
            std::memcpy(buffer[ch], src, frames_to_copy * sizeof(float));
        }
        return frames_to_copy;
    }
    // 不清理目标中未覆盖的额外声道，上层混音前需保证其初始状态。
    // 多声道到较少声道只取前若干路，不进行混音矩阵或音量归一化。
    const uint16_t channels_to_copy =
        std::min((uint16_t)data.pcm_data.size(), num_channels);
    // memcpy 要求源目标不重叠；调用方不能拿 origin 返回的缓存视图作为可写目标。
    for ( uint16_t ch = 0; ch < channels_to_copy; ++ch ) {
        const float* src  = data.pcm_data[ch].data() + start_frame;
        float*       dest = buffer[ch];
        std::memcpy(dest, src, frames_to_copy * sizeof(float));
    }
    // 返回的是切片帧数而非全部声道样本数，空声道请求已在入口排除。
    return frames_to_copy;
}

/// @brief 追加缓存的零拷贝切片，返回每声道视图长度。
/// @param origin_data 累加视图的容器，其已有视图不被替换。
/// @param start_frame 缓存中的切片起点，超过末尾时容器保持不变。
/// @param frame_count 每声道请求帧数，裁剪至剩余长度。
/// @return 本次追加的每声道长度，所有视图共同借用解码器存储。
/// @warning 可能扩容输出 vector；首调用还可能等待，不能视作实时安全接口。
size_t CachyDecoder::origin(std::vector<std::span<const float>>& origin_data,
                            size_t start_frame, size_t frame_count)
{
    // 零帧请求不追加视图，也不承担首次结果提取与等待的职责。
    if ( frame_count == 0 ) return 0;

    // 不复制 PCM 仍需先等待缓存发布，零拷贝不代表无阻塞。
    const auto& data = get_data();
    if ( data.pcm_data.empty() ) return 0.;

    // 各声道视图共享同一个帧范围，依赖缓存各声道等长。
    const size_t total_frames = data.pcm_data[0].size();
    // 越界时连空视图也不追加，调用方应检查本次请求的返回帧数。
    if ( start_frame >= total_frames ) return 0.;

    // 请求零帧保持输出容器原样，不追加空 span 作为占位。
    const size_t frames_available = total_frames - start_frame;
    const auto   framesneeded     = std::min(frame_count, frames_available);
    if ( framesneeded == 0 ) {
        return 0.;
    }

    // 这里只追加而不清空，复用容器时调用方负责移除上一次的视图。
    // span 不拥有缓存，解码器销毁后所有返回视图都失效。
    // 只借用样本，不延长解码器生命周期；调用方需另外保证所有者存活。
    for ( const auto& chdata : data.pcm_data ) {
        origin_data.emplace_back(chdata.begin() + start_frame,
                                 chdata.begin() + start_frame + framesneeded);
    }
    return framesneeded;
}
}  // namespace ice
