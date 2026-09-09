#pragma once

#include <cstddef>
#include <ice/manage/AudioFormat.hpp>
#include <ice/manage/dec/AlbumArt.hpp>
#include <string>

namespace ice
{
/// @brief 探测得到的媒体描述值，不持有解码器或活动播放游标。
/// 文本字段可能为空；调用方只有在 probe 成功后才能消费数值元数据。
/// 总帧数属于探测元信息，实际解码长度由解码器报告。
struct MediaInfo {
    /// @brief 标题标签，缺失时为空。
    std::string title;
    /// @brief 后端提供的艺术家标签。
    std::string artist;
    /// @brief 专辑标签。
    std::string album;
    /// @brief 文件探测到的音频格式，可能不同于解码器目标输出格式。
    AudioDataFormat format;
    /// @brief 探测码率，默认零表示尚无有效码率信息。
    size_t bitrate{ 0 };
    /// @brief 媒体帧数估计，不能直接作为目标采样率下的输出容量。
    /// 默认零表示未取得长度。
    size_t frame_count{ 0 };
    /// @brief 封面以值语义随媒体信息保存，不依赖探测上下文继续存活。
    AlbumArt cover;
};
}  // namespace ice
