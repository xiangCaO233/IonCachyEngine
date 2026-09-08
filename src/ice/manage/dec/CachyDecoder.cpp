#include <ice/execptions/instance_build_error.hpp>
#include <ice/manage/dec/CachyDecoder.hpp>
#include <ice/manage/dec/IDecoderInstance.hpp>

#include <algorithm>
#include <cstring>
#include <memory>
#include <span>

#include "ice/execptions/load_error.hpp"

namespace ice
{
/// @brief 接管任务句柄，不在构造阶段等待整文件解码。
CachyDecoder::CachyDecoder(std::future<DecodedData> future_data)
    : future_data_(std::move(future_data))
{
}

/// @brief 构建后台整文件解码任务并返回缓存策略。
/// @param path 提交时复制的路径，后台任务不依赖调用方字符串存储。
/// @param target_format 交给实例的输出格式请求，不是缓存格式已满足的证明。
/// @param thread_pool 负责最终执行任务的外部线程池。
/// @param factory 在后台创建解码实例的非空工厂。
/// @return 只包含异步结果句柄的缓存策略，媒体成功与否在消费结果时确定。
/// @warning 文件访问和内存增长发生在工作线程，首次消费仍可能等待。
std::unique_ptr<CachyDecoder> CachyDecoder::create(
    std::string_view path, const ice::AudioDataFormat& target_format,
    ThreadPool& thread_pool, std::shared_ptr<IDecoderFactory> factory)
{
    // 路径复制到任务内，避免调用方 string_view 在异步执行前失效。
    // 工厂按共享所有权捕获，覆盖整个后台解码期间的生命周期。
    // 任务不捕获解码器自身；销毁策略对象不会向后台任务发送取消请求。
    auto decode_task = [target_format,
                        factory,
                        path_str = std::string(path)]() -> DecodedData {
        // 工厂必须非空；这里只检查其返回实例，不检查捕获的工厂指针。
        auto worker = factory->create_instance(path_str, target_format);
        if ( !worker ) {
            throw ice::instance_build_error("create decoder instance " +
                                            path_str + "failed");
        }

        // 使用实例报告的输出格式分配平面缓冲，不能只按目标声道数猜测。
        // 此层不再次核对采样率与目标一致性，转换结果依赖实例契约。
        auto   format       = worker->get_source_format();
        size_t total_frames = worker->get_source_total_frames();

        // 总帧数只用于容量预估；最终长度以实际读出的帧数为准。
        // 预估帧数可能很大，reserve
        // 没有应用层内存上限；实际追加还可能继续增长。
        std::vector<std::vector<float>> pcm(format.channels);
        for ( auto& channel : pcm ) {
            channel.reserve(total_frames);
        }

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
            throw ice::load_error("seek audio file " + path_str + "failed");
        }

        // 只追加实际读出的部分，最后一块不足 CHUNK_SIZE 时不补齐缓存。
        // read 返回零就结束，没有独立错误码区分正常 EOF 与解码失败。
        // 实例必须保证返回值不大于请求容量；这里没有再次校验迭代器上界。
        size_t frames_read = 0;
        while ( (frames_read = worker->read(chunk_ptrs.data(), CHUNK_SIZE)) >
                0 ) {
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
            const std::vector<float> mono_pcm = pcm[0];
            pcm.resize(target_format.channels);
            for ( uint16_t ch = 1; ch < target_format.channels; ++ch ) {
                pcm[ch] = mono_pcm;
            }
            format.channels = target_format.channels;
        }

        // 连同格式一次性移交最终存储，不让消费侧观察仍在增长的中间缓存。
        return { format, std::move(pcm) };
    };

    // 使用调用方线程池；返回后不能假设后台任务已完成。
    auto future_result = thread_pool.enqueue(decode_task);

    // 返回对象保存 future，结果只能在 get_data 中被一次性提取。
    // 已入队之后若策略对象构造失败，后台任务也不会因此自动撤回。
    return std::unique_ptr<CachyDecoder>(
        new CachyDecoder(std::move(future_result)));
}

/// @brief 首次取得任务结果，后续访问复用缓存。
/// @warning call_once 与 future.get 均可能等待，只可在准备阶段首次调用。
/// 当前历史错误处理将 std::exception 折叠为空缓存，调用方无法据此区分具体错误。
const CachyDecoder::DecodedData& CachyDecoder::get_data() const
{
    std::call_once(data_ready_flag_, [this]() {
        try {
            // call_once 确保仅一个线程消费
            // future，其余首次读取者等待同一结果发布。
            data_cache_ = future_data_.get();
        } catch ( const std::exception& e ) {
            // 固定为空结果后不重新读取 future，也不会自动重试文件。
            // 异常未逃出初始化函数时 once_flag
            // 视作完成，后续调用不会再次尝试任务。
            data_cache_ = DecodedData{};
        }
    });
    return *data_cache_;
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
    // 返回的是切片帧数而非写入声道数，num_channels 为零时也会返回这个长度。
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
