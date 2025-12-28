# 3rdpty/sources/cmake/BuildSDL.cmake

# 定义源码路径
set(SDL_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../sources/sdl")

# 覆盖 SDL3 的内部选项
set(SDL_SHARED
    OFF
    CACHE BOOL "Build SDL3 as a shared library" FORCE)
set(SDL_STATIC
    ON
    CACHE BOOL "Build SDL3 as a static library" FORCE)

# 关闭所有我们不需要的附加目标
set(SDL_TESTS
    OFF
    CACHE BOOL "Disable SDL3 tests" FORCE)
set(SDL_EXAMPLES
    OFF
    CACHE BOOL "Disable SDL3 examples" FORCE)
set(SDL_INSTALL
    OFF
    CACHE BOOL "Disable SDL3 install target" FORCE)

set(SDL_UNIX_CONSOLE_BUILD
    ON
    CACHE BOOL "Build for console, skip windowing deps" FORCE)

# 精简子系统 我们只为音频引擎保留最核心的模块
set(SDL_AUDIO
    ON
    CACHE BOOL "Enable Audio subsystem" FORCE)
set(SDL_EVENTS
    ON
    CACHE BOOL "Enable Events subsystem" FORCE)
set(SDL_TIMERS
    ON
    CACHE BOOL "Enable Timers subsystem" FORCE)
set(SDL_THREADS
    ON
    CACHE BOOL "Enable Threads subsystem" FORCE)
set(SDL_POWER
    ON
    CACHE BOOL "Enable Power management subsystem" FORCE) # 某些音频后端可能需要

# 将 SDL3 作为子项目包含进来
add_subdirectory(${SDL_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR}/sdl_build
                 EXCLUDE_FROM_ALL)

set_target_properties(SDL3-static PROPERTIES POSITION_INDEPENDENT_CODE ON)
