# 3rdpty/sources/cmake/BuildOpenAL.cmake

# 定义源码路径
set(OPENAL_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../sources/openal")

# 覆盖 OpenAL Soft 的内部选项
set(LIBTYPE
    "STATIC"
    CACHE STRING "Force static library build" FORCE)
set(ALSOFT_UTILS
    OFF
    CACHE BOOL "Disable OpenAL utils" FORCE)
set(ALSOFT_EXAMPLES
    OFF
    CACHE BOOL "Disable OpenAL examples" FORCE)
set(ALSOFT_TESTS
    OFF
    CACHE BOOL "Disable OpenAL tests" FORCE)
set(ALSOFT_INSTALL
    OFF
    CACHE BOOL "Disable OpenAL install target" FORCE)
set(ALSOFT_EMBED_HRTF_DATA
    ON
    CACHE BOOL "Embed HRTF data into the library" FORCE)

# OpenAL Soft 默认会去找 sndfile, Qt 等，我们明确告诉它不要
set(ALSOFT_NO_CONFIG_UTIL
    ON
    CACHE BOOL "Disable alsoft-config utility" FORCE)
set(ALSOFT_BACKEND_SDL2
    OFF
    CACHE BOOL "Disable SDL2 backend to avoid extra deps" FORCE)
set(ALSOFT_BACKEND_SDL3
    OFF
    CACHE BOOL "Disable SDL3 backend to avoid extra deps" FORCE)

# 将 OpenAL Soft 作为子项目包含进来

# 它会自己处理 PIC、编译器标志等
add_subdirectory(${OPENAL_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR}/openal_build
                 EXCLUDE_FROM_ALL)

# OpenAL Soft 在不同平台有不同的系统依赖，我们需要手动添加
if(APPLE)
    # 在 macOS 上，需要链接 CoreAudio 等框架
    target_link_libraries(
    OpenAL INTERFACE "-framework CoreFoundation" "-framework CoreAudio"
                     "-framework AudioToolbox")
elseif(WIN32)
    # 在 Windows 上，可能需要链接 winmm, ole32 等
    target_link_libraries(OpenAL INTERFACE winmm ole32)
else()
    # 在 Linux 上，通常需要链接 pthread 和 dl
    target_link_libraries(OpenAL INTERFACE dl)
    # 如果 ALSA 或 PulseAudio 被找到并链接，OpenAL 的 target 会自动处理 我们这里只添加最基础的依赖
endif()

set_target_properties(OpenAL PROPERTIES POSITION_INDEPENDENT_CODE ON)
