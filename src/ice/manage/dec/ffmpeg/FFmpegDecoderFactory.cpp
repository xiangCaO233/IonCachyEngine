#include <ice/manage/dec/ffmpeg/FFmpegDecoderFactory.hpp>

#include "ice/config/config.hpp"
#include "ice/manage/AudioFormat.hpp"
#include "ice/manage/dec/AlbumArt.hpp"
#include "ice/manage/dec/MediaInfo.hpp"
#include "ice/manage/dec/ffmpeg/FFmpegDecoderInstance.hpp"

extern "C" {
#include <libavformat/avformat.h>
}

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <utility>

namespace ice
{
namespace
{

/// @brief 解析探测阶段可展示的声道数。
/// @param codecParams FFmpeg 音频流参数。
/// @return 正声道数。
int probe_channel_count(const AVCodecParameters* codecParams)
{
    // 优先保留媒体自身声道数，不在元信息探测阶段强制改成混音声道数。
    if ( codecParams && codecParams->ch_layout.nb_channels > 0 ) {
        return codecParams->ch_layout.nb_channels;
    }
    // 缺失元信息时仍返回正数，避免下游按零声道准备资源。
    return std::max<int>(1,
                         static_cast<int>(ICEConfig::internal_format.channels));
}

/// @brief 解析探测阶段可展示的采样率。
/// @param codecParams FFmpeg 音频流参数。
/// @return 正采样率，源与回退配置均无效时返回零。
int probe_sample_rate(const AVCodecParameters* codecParams)
{
    // 容器中的有效采样率属于源音频；只有缺失时才使用内部配置回退。
    if ( codecParams && codecParams->sample_rate > 0 ) {
        return codecParams->sample_rate;
    }
    const uint32_t fallback = ICEConfig::internal_format.samplerate;
    // 后续时长换算需要正 int；拒绝无效回退，不将溢出值伪装为 1 Hz。
    if ( fallback == 0 ||
         fallback > static_cast<uint32_t>(std::numeric_limits<int>::max()) )
        return 0;
    return static_cast<int>(fallback);
}

/// @brief 将 FFmpeg duration 按指定 time_base 转换为目标采样率下的帧数。
/// @param duration FFmpeg duration 值。
/// @param timeBase duration 对应的时间基。
/// @param sampleRate 目标采样率。
/// @return 可用帧数；duration 无效时返回 0。
std::int64_t durationToFrameCount(std::int64_t duration, AVRational timeBase,
                                  int sampleRate)
{
    // 未知或无效时长不转为无符号帧数，防止负哨兵值变成超大容量。
    // 音频时间基必须为正有理数，缺失或异常字段不能参与整数时间换算。
    if ( duration <= 0 || sampleRate <= 0 || timeBase.num <= 0 ||
         timeBase.den <= 0 ) {
        return 0;
    }
    // 使用有理数重标度，避免先转浮点秒数后再累计舍入误差。
    return av_rescale_q(duration, timeBase, { 1, sampleRate });
}

/// @brief 在流级和容器级 duration 中选择更可靠的音频总帧数。
/// @param fmtCtx 已打开并完成 stream info 探测的 FFmpeg 上下文。
/// @param audioStream 目标音频流。
/// @param sampleRate 目标采样率。
/// @return 探测到的最大可用总帧数，未知时返回 0。
std::int64_t probeBestFrameCount(const AVFormatContext* fmtCtx,
                                 const AVStream* audioStream, int sampleRate)
{
    // 流与容器分别使用自己的时间基，不能直接比较原始 duration 数值。
    const std::int64_t streamFrames =
        audioStream
            ? durationToFrameCount(
                  audioStream->duration, audioStream->time_base, sampleRate)
            : 0;
    const std::int64_t formatFrames =
        fmtCtx ? durationToFrameCount(
                     fmtCtx->duration, { 1, AV_TIME_BASE }, sampleRate)
               : 0;
    // 取较大估算防止部分容器的流时长只覆盖首包，最终仍以实际解码为准。
    return std::max(streamFrames, formatFrames);
}

}  // namespace

/// @brief 探测首个音频流以及容器标签、封面和时长估算。
/// @warning 同步文件访问及内存分配，必须在资源加载线程调用。
/// 成功时完整替换输出，失败时保留调用方原有元信息。
bool FFmpegDecoderFactory::probe(std::string_view file_path,
                                 MediaInfo&       media_info) const
{
    // C 接口按零字节终止路径，拒绝不能完整传递的名字并保留旧输出。
    if ( file_path.empty() || file_path.find('\0') != std::string_view::npos )
        return false;
    // 工厂仅构造媒体值，不依赖音轨的播放策略或运行状态。
    // 候选从空标签和空封面开始，缺失字段不能沿用上一文件的内容。
    MediaInfo candidate;
    // string_view 不保证以零结尾，先持有副本再传给 C 接口。
    const std::string path_str(file_path);
    AVFormatContext*  fmt_ctx{ nullptr };

    if ( avformat_open_input(&fmt_ctx, path_str.c_str(), nullptr, nullptr) <
         0 ) {
        return false;
    }

    /// @brief 释放已打开的输入，关闭函数同时回收其内部流与字典。
    const auto closeInput = [](AVFormatContext* context) noexcept {
        avformat_close_input(&context);
    };
    // 所有退出路径共用所有权守卫，后续标准容器分配失败也不会遗漏关闭。
    const std::unique_ptr<AVFormatContext, decltype(closeInput)> inputOwner(
        fmt_ctx, closeInput);

    // 打开成功不代表流参数齐全，流信息探测失败也必须关闭输入。
    if ( avformat_find_stream_info(fmt_ctx, nullptr) < 0 ) {
        return false;
    }

    // 选择首个音频流，与解码实例的单音频流读取策略对应。
    int audio_stream_index{ -1 };
    for ( int i = 0; i < fmt_ctx->nb_streams; i++ ) {
        // 封面可能也是流，但只有音频类型可作为播放元信息来源。
        if ( fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO ) {
            audio_stream_index = i;
            break;
        }
    }
    // 没有音频流时拒绝候选，调用方保留上次有效结果。
    if ( audio_stream_index == -1 ) {
        // 探测失败交由调用方处理，不在库内输出文件路径。
        return false;
    }

    // 以下引用仅在所有权守卫存活时有效，公开结果保存数值及字符串副本。
    // 获取音频流的参数
    auto& audio_stream = fmt_ctx->streams[audio_stream_index];
    auto& codec_params = audio_stream->codecpar;

    const int channels = probe_channel_count(codec_params);
    // 元信息字段不能表示的声道数必须拒绝，不能窄化成另一种有效格式。
    if ( channels > std::numeric_limits<uint16_t>::max() ) return false;
    candidate.format.channels = static_cast<uint16_t>(channels);
    const int sampleRate      = probe_sample_rate(codec_params);
    // 缺少可靠时基时不发布估计结果，失败仍保留调用方已有元信息。
    if ( sampleRate == 0 ) return false;
    candidate.format.samplerate = static_cast<uint32_t>(sampleRate);

    // 获取总采样数。部分容器的 stream duration 会短到只有首个 packet
    // 附近，需与容器级 duration 比较后取更可信的较大值。
    const std::int64_t bestFrameCount = probeBestFrameCount(
        fmt_ctx, audio_stream, static_cast<int>(candidate.format.samplerate));
    // 只把有效正值写入 size_t，未知时长保留零这一约定。
    if ( bestFrameCount > 0 ) {
        candidate.frame_count = static_cast<size_t>(bestFrameCount);
    } else {
        // 缺失时长属于可接受的元信息状态，无需在终端打印提示。
        // 零是未能估算时长，不证明实际音频一定为空。
        candidate.frame_count = 0;
    }

    // 保存容器报告的码率，不用采样率和浮点 PCM 位宽推算压缩码率。
    // 非正码率视为未知；先限制正值范围，再转为平台相关的 size_t。
    candidate.bitrate = fmt_ctx->bit_rate > 0
                            ? static_cast<size_t>(std::min<uint64_t>(
                                  static_cast<uint64_t>(fmt_ctx->bit_rate),
                                  std::numeric_limits<size_t>::max()))
                            : 0;
    // 只读取实际存在的字典项，缺失标签保持候选的空值。
    const AVDictionaryEntry* tag = nullptr;

    // 字符串深拷贝必须在容器关闭前完成，不保留字典条目的 char 指针。
    // 获取艺术家
    tag = av_dict_get(
        fmt_ctx->metadata, "artist", nullptr, AV_DICT_IGNORE_SUFFIX);
    if ( tag ) {
        candidate.artist = std::string(tag->value);
    }

    // 获取专辑
    tag =
        av_dict_get(fmt_ctx->metadata, "album", nullptr, AV_DICT_IGNORE_SUFFIX);
    if ( tag ) {
        candidate.album = std::string(tag->value);
    }

    // 获取标题
    tag =
        av_dict_get(fmt_ctx->metadata, "title", nullptr, AV_DICT_IGNORE_SUFFIX);
    if ( tag ) {
        candidate.title = std::string(tag->value);
    }

    // 只提取嵌入的压缩封面包，不在音频探测阶段解码图像像素。
    // 查找并提取专辑封面
    for ( unsigned int i = 0; i < fmt_ctx->nb_streams; ++i ) {
        const AVStream* stream = fmt_ctx->streams[i];
        if ( stream->disposition & AV_DISPOSITION_ATTACHED_PIC ) {
            const AVPacket& packet = stream->attached_pic;
            // 跳过空封面流，继续寻找第一个有实际字节的附图。
            if ( packet.size > 0 && packet.data ) {
                // 深拷贝后封面归 MediaInfo 管理，输入关闭不会使其失效。
                // 替换入口接管独立副本，旧封面由容器自动回收。
                if ( !candidate.cover.assign(std::span<const uint8_t>(
                         packet.data, static_cast<size_t>(packet.size))) ) {
                    return false;
                }
                // 找到首个有效封面即停止，不将多个附图合并为一个字节流。
                break;
            }
        }
    }
    // 所有需要的数据都已复制完成，关闭输入不影响返回对象。
    // 全部资源读取成功后一次性发布，替换时自动回收旧封面存储。
    media_info = std::move(candidate);
    return true;
}

/// @brief 为每次解码建立独立上下文，避免多个音轨共享可变解码状态。
/// @warning 构造会打开媒体并分配转换资源，C 接口失败返回空实例。
std::unique_ptr<IDecoderInstance> FFmpegDecoderFactory::create_instance(
    std::string_view file_path, const ice::AudioDataFormat& target_format) const
{
    // 探测与实际解码分开打开文件，调用方应允许期间媒体被移走或改变。
    auto instance =
        std::make_unique<FFmpegDecoderInstance>(file_path, target_format);
    // 丢弃部分初始化对象会释放已取得的全部媒体句柄，不向调用方暴露半成品。
    // 成功状态独立于时长估计，合法但时长未知的媒体仍可创建实例。
    // 无须先调用 probe 判定：两次打开之间文件可能改变，必须检查本次初始化。
    if ( !instance->isValid() ) return {};
    return instance;
}

}  // namespace ice
