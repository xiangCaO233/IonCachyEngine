cmake_minimum_required(VERSION 3.31)

project(IonCachyEngin)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(SOURCES_BUILD "Build Deps From Sources" ON)

add_subdirectory(3rdpty)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include("configure-project")

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

add_executable(testIonCachyEngin ${SOURCES})

target_link_libraries(
  testIonCachyEngin PUBLIC fmt::fmt OpenAL::OpenAL SDL3::SDL3 3rd_ffmpeg
                           3rd_rubberband)

target_link_libraries(
  IonCachyEngin-dev PUBLIC fmt::fmt OpenAL::OpenAL SDL3::SDL3 3rd_ffmpeg
                           3rd_rubberband)
