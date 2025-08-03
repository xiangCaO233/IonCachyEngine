#include <ice/execptions/instance_build_error.hpp>
#include <ice/manage/dec/CachyDecoder.hpp>
#include <ice/manage/dec/IDecoderInstance.hpp>
#include <memory>
#include <span>

#include "ice/execptions/load_error.hpp"

namespace ice {
// 构造函数只保存 future
CachyDecoder::CachyDecoder(std::future<DecodedData> future_data)
    : future_data_(std::move(future_data)) {}

// 创建并提交异步任务
std::unique_ptr<CachyDecoder> CachyDecoder::create(
    std::string_view path, const ice::AudioDataFormat& target_format,
    ThreadPool& thread_pool, std::shared_ptr<IDecoderFactory> factory) {
    // 创建一个解码任务的lambda
    auto decode_task = [target_format, factory,
                        path_str = std::string(path)]() -> DecodedData {
        // 创建工人
        auto worker = factory->create_instance(path_str, target_format);
        if (!worker) {
            throw ice::instance_build_error("create decoder instance " +
                                            path_str + "failed");
        }

        // 获取原始文件格式信息
        auto format = worker->get_source_format();
        size_t total_frames = worker->get_source_total_frames();

        // 预分配内存
        std::vector<std::vector<float>> pcm(format.channels);
        for (auto& channel : pcm) {
            channel.reserve(total_frames);
        }

        // 预分配chunk缓存空间
        const size_t CHUNK_SIZE = 2048;
        std::vector<float*> chunk_ptrs(format.channels);
        std::vector<std::vector<float>> chunk_buffer(format.channels);
        for (auto& c : chunk_buffer) {
            c.resize(CHUNK_SIZE);
        }
        for (uint16_t i = 0; i < format.channels; ++i) {
            chunk_ptrs[i] = chunk_buffer[i].data();
        }

        // 定位到音频文件头
        if (!worker->seek(0)) {
            throw ice::load_error("seek audio file " + path_str + "failed");
        }

        // 循环解码整个文件
        size_t frames_read = 0;
        while ((frames_read = worker->read(chunk_ptrs.data(), CHUNK_SIZE)) >
               0) {
            for (uint16_t ch = 0; ch < format.channels; ++ch) {
                pcm[ch].insert(pcm[ch].end(), chunk_buffer[ch].begin(),
                               chunk_buffer[ch].begin() + frames_read);
            }
        }

        // 拥有返回结果的lambda
        return {format, std::move(pcm)};
    };

    // 将任务提交到全局线程池，并获取一个future
    auto future_result = thread_pool.enqueue(decode_task);

    // 返回持有此future的CachyDecoder
    return std::unique_ptr<CachyDecoder>(
        new CachyDecoder(std::move(future_result)));
}

// 当第一次需要数据时,阻塞等待 future 完成
const CachyDecoder::DecodedData& CachyDecoder::get_data() const {
    std::call_once(data_ready_flag_, [this]() {
        try {
            // future.get() 阻塞,只能调用一次
            // 会等到解码完成
            data_cache_ = future_data_.get();
        } catch (const std::exception& e) {
            // 解码失败
            // 放置空数据
            data_cache_ = DecodedData{};
        }
    });
    return *data_cache_;
}

double CachyDecoder::decode(float** buffer, uint16_t num_channels,
                            size_t start_frame, size_t frame_count) {
    // 确保后台任务已完成
    const auto& data = get_data();

    if (data.pcm_data.empty()) return 0;

    // 写入数据到buffer中,并返回此次实际写入的帧数(可能到末尾实际写入帧数不=frame_count)
    // 如果内部没有数据,或请求的起始点已超出范围，则直接返回
    const size_t total_frames = data.pcm_data[0].size();
    if (start_frame >= total_frames) return 0;

    // 确定要拷贝的帧数:取请求帧数和剩余帧数中的较小值
    const size_t frames_available = total_frames - start_frame;
    const auto frames_to_copy = std::min(frame_count, frames_available);
    if (frames_to_copy == 0) {
        return 0;
    }
    // 确定要拷贝的声道数:取源和目标声道数中的较小值
    const uint16_t channels_to_copy =
        std::min((uint16_t)data.pcm_data.size(), num_channels);
    // 逐声道进行内存拷贝
    for (uint16_t ch = 0; ch < channels_to_copy; ++ch) {
        // 源指针:指向内部PCM数据的正确起始位置
        const float* src = data.pcm_data[ch].data() + start_frame;
        // 目标指针:从传入的指针数组中获取
        float* dest = buffer[ch];
        // 块拷贝内存
        memcpy(dest, src, frames_to_copy * sizeof(float));
    }
    // 返回实际处理的帧数
    return frames_to_copy;
}

// 获取原始数据接口
double CachyDecoder::origin(std::vector<std::span<const float>>& origin_data,
                            size_t start_frame, size_t frame_count) {
    // 确保后台任务已完成
    const auto& data = get_data();
    if (data.pcm_data.empty()) return 0.;

    // 如果内部没有数据,或请求的起始点已超出范围，则直接返回
    const size_t total_frames = data.pcm_data[0].size();
    if (start_frame >= total_frames) return 0.;

    // 确定帧数:取请求帧数和剩余帧数中的较小值
    const size_t frames_available = total_frames - start_frame;
    const auto framesneeded = std::min(frame_count, frames_available);
    if (framesneeded == 0) {
        return 0.;
    }

    // 生成span
    for (const auto& chdata : data.pcm_data) {
        origin_data.emplace_back(chdata.begin() + start_frame,
                                 chdata.begin() + start_frame + framesneeded);
    }
    return framesneeded;
}
}  // namespace ice
