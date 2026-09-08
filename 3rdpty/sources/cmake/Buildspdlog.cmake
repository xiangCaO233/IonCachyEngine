# 3rdpty/sources/cmake/Buildspdlog.cmake

# 本脚本只配置上游构建选项，必须由 SOURCES_BUILD=ON 的依赖入口显式包含。 路径以调用目录为基准，不能从其他目录单独 include
# 后仍假设指向同一源码。
set(SPDLOG_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../sources/spdlog")

# 关闭示例、测试和基准目标，依赖构建只提供引擎需要的日志实现。
set(SPDLOG_BUILD_EXAMPLE
    OFF
    CACHE BOOL "Disable spdlog example" FORCE)
set(SPDLOG_BUILD_TESTS
    OFF
    CACHE BOOL "Disable spdlog tests" FORCE)
# 基准程序不属于引擎回归测试；关闭此项不能替代 ICE 自有测试验证。
set(SPDLOG_BUILD_BENCH
    OFF
    CACHE BOOL "Disable spdlog benchmarks" FORCE)
# spdlog 跟随 ICE_LINKAGE，shared 预编译包中提供 DLL。
set(ICE_SPDLOG_BUILD_SHARED OFF)
if(ICE_LINKAGE STREQUAL "shared")
  # 强制写回上游缓存，使同一构建目录切换链接偏好时不保留旧模式。
  set(ICE_SPDLOG_BUILD_SHARED ON)
endif()
set(SPDLOG_BUILD_SHARED
    ${ICE_SPDLOG_BUILD_SHARED}
    CACHE BOOL "Disable spdlog shared library" FORCE)
# 缓存说明文字沿用历史值，实际开关以 ICE_LINKAGE 推导结果为准。 这里作为嵌入式依赖使用，不向宿主机安装另一份全局 spdlog 包。
set(SPDLOG_INSTALL
    OFF
    CACHE BOOL "Disable spdlog install target" FORCE)

# 与预编译模式一致使用外部 fmt，防止日志头内嵌另一套格式化实现。
set(SPDLOG_FMT_EXTERNAL
    ON
    CACHE BOOL "Use external fmt library" FORCE)

# 即使当前生成静态归档也启用 PIC，便于之后链接进入共享引擎。
set(SPDLOG_BUILD_PIC
    ON
    CACHE BOOL "Build Position Independent Code" FORCE)

# 调用入口先包含 Buildfmt，确保上游配置 spdlog 前已经有 fmt::fmt。 EXCLUDE_FROM_ALL
# 不取消依赖链接时的构建，SYSTEM 用于上游包含目录属性。 使用独立二进制子目录，避免将缓存和生成文件写进上游源码目录。
add_subdirectory(${SPDLOG_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR}/spdlog_build
                 EXCLUDE_FROM_ALL SYSTEM)
