cmake_minimum_required(VERSION 3.31)

project(IonCachyEngin)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
message(STATUS "ENV:$ENV{PATH}")
# 添加avx指令集编译选项
if(APPLE)

else()
  if(MSVC)
    # add_compile_options("/arch:AVX2")
  else()
    add_compile_options("-mavx2")
  endif()
endif(APPLE)

# 运行时内存检查 set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -g -fsanitize=address")
# set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -g -fsanitize=address")
# set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fsanitize=address")

# 日志库
find_package(spdlog REQUIRED)
# 格式化库
find_package(fmt REQUIRED)
# openal
find_package(OpenAL REQUIRED)
# (c包管理器)
find_package(PkgConfig REQUIRED)
# sdl
find_package(SDL3 REQUIRED)

# rubberband库
pkg_check_modules(rubberband REQUIRED IMPORTED_TARGET rubberband)
if(CMAKE_CROSSCOMPILING)
    get_target_property(ORIGINAL_LINK_FLAGS PkgConfig::rubberband INTERFACE_LINK_LIBRARIES)
    message(STATUS "Cross-compiling: Original link flags from PkgConfig::rubberband: ${ORIGINAL_LINK_FLAGS}")

    set(CLEANED_RUBBERBAND_LIBRARIES "")
    
    # 遍历由分号分隔的列表中的每一项
    foreach(ITEM ${ORIGINAL_LINK_FLAGS})
        # [关键修正] 使用 MATCHES 来检查子字符串。
        # 如果这一项不包含 "-framework"，我们就保留它。
        if(NOT ITEM MATCHES "-framework")
            list(APPEND CLEANED_RUBBERBAND_LIBRARIES ${ITEM})
        endif()
    endforeach()
    
    message(STATUS "Cross-compiling: Final cleaned libraries are now: ${CLEANED_RUBBERBAND_LIBRARIES}")
else()
    # 在非交叉编译时，直接使用导入目标
    set(CLEANED_RUBBERBAND_LIBRARIES PkgConfig::rubberband)
endif()

# ffmpeg
if(WIN32)
  # find_package(FFMPEG REQUIRED)
  # set(FFLIB ${FFMPEG_LIBRARIES})
  find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED 
    libavcodec 
    libavformat 
    libavutil 
    libavdevice 
    libavfilter 
    libswscale 
    libswresample
)

# 设置变量以保持兼容性
set(FFMPEG_FOUND TRUE)
set(FFMPEG_INCLUDE_DIRS ${FFMPEG_INCLUDE_DIRS})
set(FFMPEG_LIBRARIES ${FFMPEG_LIBRARIES})
set(FFLIB ${FFMPEG_LIBRARIES})

else()
  pkg_check_modules(ffmpeg-base REQUIRED IMPORTED_TARGET libswresample
                    libavcodec libavformat)
  pkg_check_modules(ffmpeg-util REQUIRED IMPORTED_TARGET libavutil)
  set(FFLIB PkgConfig::ffmpeg-base PkgConfig::ffmpeg-util)
endif(WIN32)

set(LIB_SOURCES
    src/ice/core/SourceNode.cpp
    src/ice/core/MixBus.cpp
    src/ice/core/effect/IEffectNode.cpp
    src/ice/core/effect/Clipper.cpp
    src/ice/core/effect/Compresser.cpp
    src/ice/core/effect/TimeStretcher.cpp
    src/ice/core/effect/PitchAlter.cpp
    src/ice/core/effect/filter/BiquadFilter.cpp
    src/ice/core/effect/GraphicEqualizer.cpp
    src/ice/config/config.cpp
    src/ice/manage/AudioBuffer.cpp
    src/ice/manage/AudioTrack.cpp
    src/ice/manage/AudioPool.cpp
    src/ice/manage/dec/CachyDecoder.cpp
    src/ice/manage/dec/StreamingDecoder.cpp
    src/ice/manage/dec/ffmpeg/FFmpegDecoderFactory.cpp
    src/ice/manage/dec/ffmpeg/FFmpegDecoderInstance.cpp
    src/ice/thread/ThreadPool.cpp
    src/ice/out/IReceiver.cpp
    src/ice/out/play/openal/ALPlayer.cpp
    src/ice/out/play/sdl/SDLPlayer.cpp)

set(SOURCES ${LIB_SOURCES} src/main.cpp)

include_directories(include)

add_library(IonCachyEngin-dev STATIC ${LIB_SOURCES})

add_library(IonCachyEngin SHARED ${LIB_SOURCES})

add_executable(testIonCachyEngin ${SOURCES})

target_link_libraries(
  testIonCachyEngin PUBLIC fmt::fmt OpenAL::OpenAL ${CLEANED_RUBBERBAND_LIBRARIES}
                           SDL3::SDL3 ${FFLIB})

target_link_libraries(
  IonCachyEngin-dev PUBLIC fmt::fmt OpenAL::OpenAL ${CLEANED_RUBBERBAND_LIBRARIES}
                           SDL3::SDL3 ${FFLIB})

target_link_libraries(
  IonCachyEngin PUBLIC fmt::fmt OpenAL::OpenAL ${CLEANED_RUBBERBAND_LIBRARIES} SDL3::SDL3
                       ${FFLIB})
