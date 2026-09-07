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
/// @warning 文件访问和内存增长发生在工作线程，首次消费仍可能等待。
std::unique_ptr<CachyDecoder> CachyDecoder::create(
    std::string_view path, const ice::AudioDataFormat& target_format,
    ThreadPool& thread_pool, std::shared_ptr<IDecoderFactory> factory)
{
    // 路径复制到任务内，避免调用方 string_view 在异步执行前失效。
    // 工厂按共享所有权捕获，覆盖整个后台解码期间的生命周期。
    // 创建一个解码任务的lambda
    auto decode_task = [target_format,
                        factory,
                        path_str = std::string(path)]() -> DecodedData {
        // 创建工人
        auto worker = factory->create_instance(path_str, target_format);
        if ( !worker ) {
            throw ice::instance_build_error("create decoder instance " +
                                            path_str + "failed");
        }

        // 使用实例报告的输出格式分配平面缓冲，不能只按目标声道数猜测。
        // 获取原始文件格式信息
        auto   format       = worker->get_source_format();
        size_t total_frames = worker->get_source_total_frames();

        // 总帧数只用于容量预估；最终长度以实际读出的帧数为准。
        // 预分配内存
        std::vector<std::vector<float>> pcm(format.channels);
        for ( auto& channel : pcm ) {
            channel.reserve(total_frames);
        }

        // 每个通道拥有独立连续数组；read 需要的是指针表而非交错 PCM。
        // 预分配chunk缓存空间
        const size_t                    CHUNK_SIZE = 2048;
        std::vector<float*>             chunk_ptrs(format.channels);
        std::vector<std::vector<float>> chunk_buffer(format.channels);
        for ( auto& c : chunk_buffer ) {
            c.resize(CHUNK_SIZE);
        }
        for ( uint16_t i = 0; i < format.channels; ++i ) {
            chunk_ptrs[i] = chunk_buffer[i].data();
        }

        // 定位到音频文件头
        if ( !worker->seek(0) ) {
            throw ice::load_error("seek audio file " + path_str + "failed");
        }

        // 只追加实际读出的部分，最后一块不足 CHUNK_SIZE 时不补齐缓存。
        // 循环解码整个文件
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

        // 拥有返回结果的lambda
        return { format, std::move(pcm) };
    };

    // 使用调用方线程池；返回后不能假设后台任务已完成。
    auto future_result = thread_pool.enqueue(decode_task);

    // 返回对象保存 future，结果只能在 get_data 中被一次性提取。
    // 返回持有此future的CachyDecoder
    return std::unique_ptr<CachyDecoder>(
        new CachyDecoder(std::move(future_result)));
}

/// @brief 首次取得任务结果，后续访问复用缓存。
/// @warning call_once 与 future.get 均可能等待，只可在准备阶段首次调用。
/// 当前历史错误处理将失败折叠为空缓存，调用方无法据此区分具体错误。
const CachyDecoder::DecodedData& CachyDecoder::get_data() const
{
    std::call_once(data_ready_flag_, [this]() {
        try {
            // future.get() 阻塞,只能调用一次
            // 会等到解码完成
            data_cache_ = future_data_.get();
        } catch ( const std::exception& e ) {
            // 固定为空结果后不重新读取 future，也不会自动重试文件。
            // 解码失败
            // 放置空数据
            data_cache_ = DecodedData{};
        }
    });
    return *data_cache_;
}

/// @brief 从不可变缓存复制指定帧区间。
/// @warning 音频读取前应预先触发 get_data；此函数本身不保证首次访问无等待。
size_t CachyDecoder::decode(float** buffer, uint16_t num_channels,
                            size_t start_frame, size_t frame_count)
{
    // 确保后台任务已完成
    const auto& data = get_data();

    // 空缓存可能是零长度媒体，也可能是后台失败；此接口不区分二者。
    if ( data.pcm_data.empty() ) return 0;

    // 写入数据到buffer中,并返回此次实际写入的帧数(可能到末尾实际写入帧数不=frame_count)
    // 如果内部没有数据,或请求的起始点已超出范围，则直接返回
    const size_t total_frames = data.pcm_data[0].size();
    if ( start_frame >= total_frames ) return 0;

    // 确定要拷贝的帧数:取请求帧数和剩余帧数中的较小值
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
    // 确定要拷贝的声道数:取源和目标声道数中的较小值
    const uint16_t channels_to_copy =
        std::min((uint16_t)data.pcm_data.size(), num_channels);
    // 逐声道进行内存拷贝
    for ( uint16_t ch = 0; ch < channels_to_copy; ++ch ) {
        // 源指针:指向内部PCM数据的正确起始位置
        const float* src = data.pcm_data[ch].data() + start_frame;
        // 目标指针:从传入的指针数组中获取
        float* dest = buffer[ch];
        // 块拷贝内存
        std::memcpy(dest, src, frames_to_copy * sizeof(float));
    }
    // 返回实际处理的帧数
    return frames_to_copy;
}

/// @brief 追加缓存的零拷贝切片，返回每声道视图长度。
/// @warning 可能扩容输出 vector；首调用还可能等待，不能视作实时安全接口。
size_t CachyDecoder::origin(std::vector<std::span<const float>>& origin_data,
                            size_t start_frame, size_t frame_count)
{
    // 确保后台任务已完成
    const auto& data = get_data();
    if ( data.pcm_data.empty() ) return 0.;

    // 如果内部没有数据,或请求的起始点已超出范围，则直接返回
    const size_t total_frames = data.pcm_data[0].size();
    // 越界时连空视图也不追加，调用方应检查本次请求的返回帧数。
    if ( start_frame >= total_frames ) return 0.;

    // 确定帧数:取请求帧数和剩余帧数中的较小值
    const size_t frames_available = total_frames - start_frame;
    const auto   framesneeded     = std::min(frame_count, frames_available);
    if ( framesneeded == 0 ) {
        return 0.;
    }

    // 这里只追加而不清空，复用容器时调用方负责移除上一次的视图。
    // span 不拥有缓存，解码器销毁后所有返回视图都失效。
    // 生成span
    for ( const auto& chdata : data.pcm_data ) {
        origin_data.emplace_back(chdata.begin() + start_frame,
                                 chdata.begin() + start_frame + framesneeded);
    }
    return framesneeded;
}
}  // namespace ice
