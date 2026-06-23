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
  find_package(lame REQUIRED)
  find_package(zlib REQUIRED)
  find_package(rubberband REQUIRED)
  find_package(ffmpeg REQUIRED)
endif()
