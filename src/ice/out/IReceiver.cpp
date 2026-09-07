#include <ice/out/IReceiver.hpp>

namespace ice
{
/// @brief 保留各接收后端共同的格式参数入口，基类不保存该格式。
/// 设备打开、格式转换及容量准备由具体派生后端负责。
/// 构造不启动拉取线程，调用方仍需显式执行后端生命周期接口。
IReceiver::IReceiver([[maybe_unused]] const AudioDataFormat& format) {}
}  // namespace ice
