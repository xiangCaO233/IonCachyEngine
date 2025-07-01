cmake_minimum_required(VERSION 3.31)

project(IonCachyEngin)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_BUILD_TYPE Debug)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

#
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
  pkg_check_modules(
    ffmpeg
    REQUIRED
    IMPORTED_TARGET
    libavutil
    libswresample
    libavcodec
    libavformat)
endif(WIN32)

# set(LIB_SOURCES )
set(SOURCES src/main.cpp)

# add_library(IonCachyEngin-dev STATIC ${LIB_SOURCES})

# add_library(IonCachyEngin SHARED ${LIB_SOURCES})

add_executable(testIonCachyEngin ${SOURCES})

target_link_libraries(
  testIonCachyEngin PUBLIC fmt::fmt OpenAL::OpenAL PkgConfig::rubberband
                           PkgConfig::ffmpeg)
# if(WIN32)
#
# else()
#
# endif(WIN32)

# target_link_libraries(IonCachyEngin-dev PUBLIC )

# target_link_libraries(IonCachyEngin PUBLIC )
