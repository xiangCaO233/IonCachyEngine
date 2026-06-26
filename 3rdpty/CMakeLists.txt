# 3rdpty/CMakeLists.txt

# SOURCES_BUILD 是显式依赖来源开关：ON 使用源码构建，OFF 只使用预编译库。
if(SOURCES_BUILD)
  add_subdirectory(sources)
else()
  list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake/modules")
  find_package(fmt REQUIRED)
  find_package(spdlog REQUIRED)
  find_package(OpenAL REQUIRED)
  find_package(SDL3Static REQUIRED)
  find_package(libsamplerate REQUIRED)
  find_package(fftw REQUIRED)
  if(ICE_LINKAGE STREQUAL "static")
    # 静态 FFmpeg 预编译包需要显式传播 LAME 与 zlib；shared 包由 FFmpeg DLL 自身声明运行时依赖。
    find_package(lame REQUIRED)
    find_package(zlib REQUIRED)
  endif()
  find_package(rubberband REQUIRED)
  find_package(ffmpeg REQUIRED)
endif()
