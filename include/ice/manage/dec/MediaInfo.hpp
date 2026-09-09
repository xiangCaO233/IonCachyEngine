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
    /// @brief 探测到的标题标签，缺失时可为空，不从播放状态生成。
    std::string title;
    /// @brief 探测到的艺术家标签，原样保存后端提供的文本。
    std::string artist;
    /// @brief 探测到的专辑标签，与封面存在与否独立。
    std::string album;
    /// @brief 文件探测到的音频格式，可能不同于解码器目标输出格式。
    AudioDataFormat format;
    /// @brief 探测码率，默认零表示尚无有效码率信息。
    /// 工厂未填写该字段时仍可安全复制元信息，零值不证明探测成功。
    size_t bitrate{ 0 };
    /// @brief 媒体帧数估计，不能直接作为目标采样率下的输出容量。
    /// 默认零表示未取得长度，调用方仍须检查探测结果。
    size_t frame_count{ 0 };
    /// @brief 封面以值语义随媒体信息保存，不依赖探测上下文继续存活。
    AlbumArt cover;
};
}  // namespace ice
