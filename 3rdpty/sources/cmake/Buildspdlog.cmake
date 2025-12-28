# 3rdpty/sources/cmake/Buildspdlog.cmake

# 定义源码路径
set(SPDLOG_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../sources/spdlog")

# 覆盖 spdlog 的内部选项
set(SPDLOG_BUILD_EXAMPLE
    OFF
    CACHE BOOL "Disable spdlog example" FORCE)
set(SPDLOG_BUILD_TESTS
    OFF
    CACHE BOOL "Disable spdlog tests" FORCE)
set(SPDLOG_BUILD_BENCH
    OFF
    CACHE BOOL "Disable spdlog benchmarks" FORCE)
set(SPDLOG_BUILD_SHARED
    OFF
    CACHE BOOL "Disable spdlog shared library" FORCE)
set(SPDLOG_INSTALL
    OFF
    CACHE BOOL "Disable spdlog install target" FORCE)

# 告诉 spdlog 使用外部的 fmt
set(SPDLOG_FMT_EXTERNAL
    ON
    CACHE BOOL "Use external fmt library" FORCE)

# 确保 PIC (位置无关代码) 被启用，以便链接进我们的动态库
set(SPDLOG_BUILD_PIC
    ON
    CACHE BOOL "Build Position Independent Code" FORCE)

# 将 spdlog 作为子项目包含进来 在调用 add_subdirectory 之前，我们已经通过 3rd_fmt (fmt) 提供了 fmt::fmt
# target spdlog 的 CMake 脚本会自动 find_package(fmt) 或直接找到这个 target
add_subdirectory(${SPDLOG_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR}/spdlog_build
                 EXCLUDE_FROM_ALL)
