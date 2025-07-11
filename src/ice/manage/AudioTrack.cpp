#include <ice/manage/AudioTrack.hpp>

namespace ice {
// 私有构造函数，强制使用工厂方法
AudioTrack::AudioTrack(std::string path) : file_path(path) {}

}  // namespace ice
