cmake_minimum_required(VERSION 3.31)

project(IonCachyEngin)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_BUILD_TYPE Debug)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 日志库
find_package(spdlog REQUIRED)
# 格式化库
find_package(fmt REQUIRED)
# openal
find_package(OpenAL REQUIRED)

# (c包管理器)
find_package(PkgConfig REQUIRED)

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

# set(LIB_SOURCES )
set(SOURCES
    src/ice/core/SourceNode.cpp src/ice/core/MixBus.cpp
    src/ice/core/effect/IEffectNode.cpp src/ice/core/effect/Clipper.cpp
    src/ice/manage/AudioBuffer.cpp src/main.cpp)

include_directories(include)

# add_library(IonCachyEngin-dev STATIC ${LIB_SOURCES})

# add_library(IonCachyEngin SHARED ${LIB_SOURCES})

add_executable(testIonCachyEngin ${SOURCES})

target_link_libraries(
  testIonCachyEngin PUBLIC fmt::fmt OpenAL::OpenAL PkgConfig::rubberband
                           PkgConfig::ffmpeg-base PkgConfig::ffmpeg-util)
# if(WIN32)
#
# else()
#
# endif(WIN32)

# target_link_libraries(IonCachyEngin-dev PUBLIC )

# target_link_libraries(IonCachyEngin PUBLIC )
