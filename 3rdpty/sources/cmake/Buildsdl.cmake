# 3rdpty/sources/cmake/BuildSDL.cmake

# 定义源码路径
set(SDL_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../sources/sdl")

# 覆盖 SDL3 的内部选项
set(ICE_SDL_BUILD_SHARED OFF)
set(ICE_SDL_BUILD_STATIC ON)
if(ICE_LINKAGE STREQUAL "shared")
  set(ICE_SDL_BUILD_SHARED ON)
  set(ICE_SDL_BUILD_STATIC OFF)
endif()

set(SDL_SHARED
    ${ICE_SDL_BUILD_SHARED}
    CACHE BOOL "Build SDL3 as a shared library" FORCE)
set(SDL_STATIC
    ${ICE_SDL_BUILD_STATIC}
    CACHE BOOL "Build SDL3 as a static library" FORCE)

# --- 构建目标 ---
set(SDL_TESTS
    OFF
    CACHE BOOL "Disable SDL3 tests" FORCE)
set(SDL_X11_XTEST
    OFF
    CACHE BOOL "Disable SDL3 X11 Xtests" FORCE)
set(SDL_EXAMPLES
    OFF
    CACHE BOOL "Disable SDL3 examples" FORCE)
set(SDL_INSTALL
    OFF
    CACHE BOOL "Disable SDL3 install target" FORCE)
set(SDL_UNIX_CONSOLE_BUILD
    ON
    CACHE BOOL "Build for console, skip windowing deps" FORCE)

# --- 白名单：只开启绝对必要的核心模块 ---
set(SDL_AUDIO
    ON
    CACHE BOOL "Enable Audio subsystem" FORCE)
set(SDL_EVENTS
    ON
    CACHE BOOL "Enable Events subsystem" FORCE)
set(SDL_TIMERS
    ON
    CACHE BOOL "Enable Timers subsystem" FORCE)
set(SDL_THREADS
    ON
    CACHE BOOL "Enable Threads subsystem" FORCE)
set(SDL_POWER
    ON
    CACHE BOOL "Enable Power management subsystem" FORCE)

# --- Linux 音频后端：预编译 SDL 必须带真实播放设备支持 ---
if(LINUX)
  # PipeWire 是现代 Linux 桌面默认音频服务，PulseAudio 和 ALSA 作为广泛部署的兼容路径。
  # 动态加载可避免静态 SDL 目标把宿主机音频库路径传播给主项目链接命令。
  set(SDL_PIPEWIRE
      ON
      CACHE BOOL "Enable SDL3 PipeWire audio backend" FORCE)
  set(SDL_PIPEWIRE_SHARED
      ON
      CACHE BOOL "Dynamically load PipeWire for SDL3 audio" FORCE)
  set(SDL_PULSEAUDIO
      ON
      CACHE BOOL "Enable SDL3 PulseAudio audio backend" FORCE)
  set(SDL_PULSEAUDIO_SHARED
      ON
      CACHE BOOL "Dynamically load PulseAudio for SDL3 audio" FORCE)
  set(SDL_ALSA
      ON
      CACHE BOOL "Enable SDL3 ALSA audio backend" FORCE)
  set(SDL_ALSA_SHARED
      ON
      CACHE BOOL "Dynamically load ALSA for SDL3 audio" FORCE)
  set(SDL_JACK
      ON
      CACHE BOOL "Enable SDL3 JACK audio backend" FORCE)
  set(SDL_JACK_SHARED
      ON
      CACHE BOOL "Dynamically load JACK for SDL3 audio" FORCE)
endif()

# --- 黑名单：强制关闭所有非音频核心的子系统 ---
set(SDL_VIDEO
    OFF
    CACHE BOOL "Disable Video subsystem" FORCE)
set(SDL_RENDER
    OFF
    CACHE BOOL "Disable Render subsystem" FORCE)
set(SDL_GPU
    OFF
    CACHE BOOL "Disable GPU subsystem" FORCE)
set(SDL_CAMERA
    OFF
    CACHE BOOL "Disable Camera subsystem" FORCE)
set(SDL_JOYSTICK
    OFF
    CACHE BOOL "Disable Joystick subsystem" FORCE)
set(SDL_HAPTIC
    OFF
    CACHE BOOL "Disable Haptic subsystem" FORCE)
set(SDL_SENSOR
    OFF
    CACHE BOOL "Disable Sensor subsystem" FORCE)
set(SDL_HIDAPI
    OFF
    CACHE BOOL "Disable HIDAPI subsystem" FORCE)
set(SDL_TRAY
    OFF
    CACHE BOOL "Disable System Tray support" FORCE)
set(SDL_DIALOG
    OFF
    CACHE BOOL "Disable Dialog support" FORCE)

# --- 进一步精简：关闭所有图形和窗口系统相关的后端 ---
set(SDL_X11
    OFF
    CACHE BOOL "Disable X11 support" FORCE)
set(SDL_WAYLAND
    OFF
    CACHE BOOL "Disable Wayland support" FORCE)
set(SDL_VULKAN
    OFF
    CACHE BOOL "Disable Vulkan support" FORCE)
set(SDL_OPENGL
    OFF
    CACHE BOOL "Disable OpenGL support" FORCE)
set(SDL_OPENGLES
    OFF
    CACHE BOOL "Disable OpenGL ES support" FORCE)
set(SDL_KMSDRM
    OFF
    CACHE BOOL "Disable KMS/DRM support" FORCE)
set(SDL_DBUS
    OFF
    CACHE BOOL "Disable D-Bus support" FORCE) # DBus 主要用于桌面集成
set(SDL_IBUS
    OFF
    CACHE BOOL "Disable IBus (input method) support" FORCE)
set(SDL_LIBUDEV
    OFF
    CACHE BOOL "Disable libudev support" FORCE) # udev 用于设备热插拔，主要是输入设备

# 将 SDL3 作为子项目包含进来
add_subdirectory(${SDL_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR}/sdl_build
                 EXCLUDE_FROM_ALL SYSTEM)

add_library(3rd_sdl3 INTERFACE)
if(ICE_LINKAGE STREQUAL "shared")
  if(TARGET SDL3-shared)
    target_link_libraries(3rd_sdl3 INTERFACE SDL3-shared)
  elseif(TARGET SDL3::SDL3)
    target_link_libraries(3rd_sdl3 INTERFACE SDL3::SDL3)
  elseif(TARGET SDL3)
    target_link_libraries(3rd_sdl3 INTERFACE SDL3)
  else()
    message(FATAL_ERROR "SDL3 shared target not found.")
  endif()
else()
  set_target_properties(SDL3-static PROPERTIES POSITION_INDEPENDENT_CODE ON)
  target_link_libraries(3rd_sdl3 INTERFACE SDL3-static)
endif()
target_include_directories(
  3rd_sdl3 INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/../sources/sdl/include")
