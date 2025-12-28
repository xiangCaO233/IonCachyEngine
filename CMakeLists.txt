# CMakeLists.txt

cmake_minimum_required(VERSION 3.22)

project(IonCachyEngine LANGUAGES CXX C)

# 全局设置
set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include("configure-project")

# 选项定义
option(ICE_BUILD_TESTS "Build the 'testIonCachyEngin' example program." ON)
option(SOURCES_BUILD "Build Deps from sources." ON)

# 包含第三方依赖 3rdpty/CMakeLists.txt 将负责创建依赖库
add_subdirectory(3rdpty)

# 包含源码目录 真正的库和可执行文件定义在 src/CMakeLists.txt 中
add_subdirectory(src)
