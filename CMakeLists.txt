cmake_minimum_required(VERSION 3.31)

project(IonCachyEngin)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 添加avx指令集编译选项
if(APPLE)

else()
  add_compile_options(-mavx2)
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

# ffmpeg
if(WIN32)
  find_package(FFMPEG REQUIRED)
else()
  pkg_check_modules(ffmpeg-base REQUIRED IMPORTED_TARGET libswresample
                    libavcodec libavformat)
  pkg_check_modules(ffmpeg-util REQUIRED IMPORTED_TARGET libavutil)
endif(WIN32)

set(LIB_SOURCES
    src/ice/core/SourceNode.cpp
    src/ice/core/MixBus.cpp
    src/ice/core/effect/IEffectNode.cpp
    src/ice/core/effect/Clipper.cpp
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
    src/ice/out/play/sdl/SDLPlayer.cpp)

set(SOURCES ${LIB_SOURCES} src/main.cpp)

include_directories(include)

add_library(IonCachyEngin-dev STATIC ${LIB_SOURCES})

add_library(IonCachyEngin SHARED ${LIB_SOURCES})

add_executable(testIonCachyEngin ${SOURCES})

target_link_libraries(
  testIonCachyEngin
  PUBLIC fmt::fmt OpenAL::OpenAL PkgConfig::rubberband SDL3::SDL3
         PkgConfig::ffmpeg-base PkgConfig::ffmpeg-util)
# if(WIN32)
#
# else()
#
# endif(WIN32)

target_link_libraries(
  IonCachyEngin-dev
  PUBLIC fmt::fmt OpenAL::OpenAL PkgConfig::rubberband SDL3::SDL3
         PkgConfig::ffmpeg-base PkgConfig::ffmpeg-util)

target_link_libraries(
  IonCachyEngin PUBLIC fmt::fmt OpenAL::OpenAL PkgConfig::rubberband SDL3::SDL3
                       PkgConfig::ffmpeg-base PkgConfig::ffmpeg-util)
