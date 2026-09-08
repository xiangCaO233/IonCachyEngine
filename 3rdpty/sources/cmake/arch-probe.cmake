# 3rdpty/sources/arch-probe.cmake

# 将 CMake 目标架构归一化为 FFmpeg 配置使用的名称，交叉编译时不读取宿主 CPU。
if(CMAKE_SYSTEM_PROCESSOR MATCHES "(x86_64|AMD64)")
  set(FFMPEG_ARCH "x86_64")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "(arm64|aarch64)")
  set(FFMPEG_ARCH "arm64")
else()
  # 未识别架构原样传递，由 FFmpeg 配置阶段判断支持性，不自动猜测位宽。
  set(FFMPEG_ARCH "${CMAKE_SYSTEM_PROCESSOR}")
endif()

# 输出目标 OS 字符串供后续 configure 参数使用，不在此运行编译探测程序。
if(APPLE)
  set(FFMPEG_TARGET_OS "darwin")
elseif(WIN32)
  # Windows 下 FFmpeg 通常区分 mingw32 或 win32/win64 (msvc)
  if(MSVC)
    # 当前 MSVC 分支固定 win64；不意味着通用支持所有 Windows 指针宽度。
    set(FFMPEG_TARGET_OS "win64")
    set(FFMPEG_ADDITIONAL_CONF "--toolchain=msvc")
  else()
    # mingw32 是 FFmpeg 的工具链目标名，64 位 MinGW 也使用该 OS 名称。
    set(FFMPEG_TARGET_OS "mingw32")
  endif()
else()
  # 非 Apple/Windows 均按 Linux 处理，新平台需显式扩展，不能假定自动兼容。
  set(FFMPEG_TARGET_OS "linux")
endif()

# ADDITIONAL_CONF 只在 MSVC 分支赋值；调用者重复 include 时需避免遗留不同目标状态。
# 配置日志展示实际传入值，便于核对交叉编译目标是否与预编译布局一致。
message(
  STATUS "FFmpeg Cross-Config: Arch=${FFMPEG_ARCH}, OS=${FFMPEG_TARGET_OS}")
