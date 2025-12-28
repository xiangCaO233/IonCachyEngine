# 3rdpty/sources/arch-probe.cmake

# 探测 架构 (Arch) CMake 的 CMAKE_SYSTEM_PROCESSOR

# 在不同平台返回不同（x86_64, AMD64, arm64, aarch64）
if(CMAKE_SYSTEM_PROCESSOR MATCHES "(x86_64|AMD64)")
    set(FFMPEG_ARCH "x86_64")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "(arm64|aarch64)")
    set(FFMPEG_ARCH "arm64")
else()
    # 兜底方案
    set(FFMPEG_ARCH "${CMAKE_SYSTEM_PROCESSOR}")
endif()

# 探测 操作系统 (Target OS)
if(APPLE)
    set(FFMPEG_TARGET_OS "darwin")
elseif(WIN32)
    # Windows 下 FFmpeg 通常区分 mingw32 或 win32/win64 (msvc)
    if(MSVC)
        set(FFMPEG_TARGET_OS "win64")
        set(FFMPEG_ADDITIONAL_CONF "--toolchain=msvc")
    else()
        set(FFMPEG_TARGET_OS "mingw32")
    endif()
else()
    # 默认 Linux
    set(FFMPEG_TARGET_OS "linux")
endif()

# 打印结果，方便在编译时 debug
message(
  STATUS "FFmpeg Cross-Config: Arch=${FFMPEG_ARCH}, OS=${FFMPEG_TARGET_OS}")
