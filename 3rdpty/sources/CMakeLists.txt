# 3rdpty/sources/CMakeLists.txt

# 仅由上层 SOURCES_BUILD=ON 分支进入；预编译模式不会用缺包作为进入此处的理由。
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# 先生成 FFmpeg 使用的目标架构和 OS 参数，随后各库脚本创建明确的消费目标。
include(arch-probe)
# 压缩与 MP3 编码目标必须先于 FFmpeg 建立，供其配置和最终链接闭包引用。
include(Buildzlib)
include(Buildlame)
include(Buildffmpeg)
# Rubber Band 依赖 FFTW 和采样率转换，前两者的目标须先完成配置。
include(Buildfftw)
include(Buildlibsamplerate)
include(Buildrubberband)
# spdlog 使用外部 fmt；包含顺序保证上游配置日志库时能找到同一格式化目标。
include(Buildfmt)
include(Buildspdlog)
# 输出后端在此只加入构建，不初始化声卡；设备资源由引擎运行期管理。
include(Buildopenal)
include(Buildsdl)
