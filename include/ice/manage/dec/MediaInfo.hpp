#include <ice/manage/AudioFormat.hpp>
#include <ice/manage/dec/AlbumArt.hpp>
#include <string>

namespace ice
{
/// @brief 探测得到的媒体描述值，不持有解码器或活动播放游标。
/// 文本字段可能为空；调用方只有在 probe 成功后才能消费数值元数据。
/// 总帧数属于探测元信息，实际解码长度由解码器报告。
struct MediaInfo {
    std::string title;
    std::string artist;
    std::string album;
    /// @brief 文件探测到的音频格式，可能不同于解码器目标输出格式。
    AudioDataFormat format;
    /// @brief 探测码率；无法从文件取得时由具体探测实现决定回退值。
    size_t bitrate;
    /// @brief 媒体帧数估计，不能直接作为目标采样率下的输出容量。
    size_t frame_count;
    /// @brief 封面以值语义随媒体信息保存，不依赖探测上下文继续存活。
    AlbumArt cover;
};
}  // namespace ice
