cmake_minimum_required(VERSION 3.31)

project(IonCachyEngin)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

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
    # 修复 rubberband 的包含路径
    set_target_properties(PkgConfig::rubberband PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_SYSROOT}/include"
    )

    # ==================== 这是新增的关键修正 ====================
    # 我们将清理掉从主机 pkg-config 带来的不兼容的链接库/标志
    
    # 首先，获取被污染的链接库列表
    get_target_property(RUBBERBAND_LIBS PkgConfig::rubberband INTERFACE_LINK_LIBRARIES)
    message(STATUS "Original rubberband libs (from host pkg-config): ${RUBBERBAND_LIBS}")

    # 创建一个新的、干净的列表
    set(CLEANED_RUBBERBAND_LIBS "")
    
    # 遍历所有被污染的库/标志
    foreach(LIB ${RUBBERBAND_LIBS})
        # 如果某个条目不是 "-framework Accelerate"，我们就把它加入到干净列表中
        # 注意：这里需要精确匹配
        if(NOT (LIB STREQUAL "-framework" OR LIB STREQUAL "Accelerate"))
            list(APPEND CLEANED_RUBBERBAND_LIBS ${LIB})
        endif()
    endforeach()
    
    # 用我们清理过的列表，覆盖掉原来的 INTERFACE_LINK_LIBRARIES 属性
    set_target_properties(PkgConfig::rubberband PROPERTIES
        INTERFACE_LINK_LIBRARIES "${CLEANED_RUBBERBAND_LIBS}"
    )
    
    message(STATUS "Cleaned rubberband libs for cross-compiling: ${CLEANED_RUBBERBAND_LIBS}")
    # ======================== 修正结束 ========================
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
  testIonCachyEngin PUBLIC fmt::fmt OpenAL::OpenAL PkgConfig::rubberband
                           SDL3::SDL3 ${FFLIB})

target_link_libraries(
  IonCachyEngin-dev PUBLIC fmt::fmt OpenAL::OpenAL PkgConfig::rubberband
                           SDL3::SDL3 ${FFLIB})

target_link_libraries(
  IonCachyEngin PUBLIC fmt::fmt OpenAL::OpenAL PkgConfig::rubberband SDL3::SDL3
                       ${FFLIB})
