# 3rdpty/CMakeLists.txt

# SOURCES_BUILD 是显式依赖来源开关：ON 使用源码构建，OFF 只使用预编译库。 两种来源必须提供相同目标名，src
# 不感知依赖来自源码还是预编译包。
if(SOURCES_BUILD)
  add_subdirectory(sources)
else()
  # 每个 Find 模块独立创建具体目标，公共布局 helper 仅解析路径。
  list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake/modules")
  # REQUIRED 保证包缺失时立即中止配置，不能带着不完整依赖继续生成。
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
  # 先解析重采样、FFT 等依赖，再创建变速器与媒体编解码目标。
  find_package(rubberband REQUIRED)
  # 共享包的运行时依赖由其自身声明，静态包则显式传递压缩辅助库。
  find_package(ffmpeg REQUIRED)
endif()
