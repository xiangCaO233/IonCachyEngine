#include <ice/manage/AudioFormat.hpp>
#include <ice/manage/dec/AlbumArt.hpp>
#include <string>

namespace ice
{
// 媒体信息
struct MediaInfo
{
    std::string     title;
    std::string     artist;
    std::string     album;
    AudioDataFormat format;
    size_t          bitrate;
    size_t          frame_count;
    // 包含封面数据
    AlbumArt cover;
};
}  // namespace ice
