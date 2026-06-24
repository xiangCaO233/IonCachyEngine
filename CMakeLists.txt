# CMakeLists.txt

cmake_minimum_required(VERSION 3.22)

project(IonCachyEngine LANGUAGES CXX C)

# 全局设置
if(APPLE
   OR MSVC
   OR (WIN32 AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU"))
  # MinGW GCC 16 的 gnu++26 与静态 libstdc++ 链接会重复定义 stdexcept 符号。 引擎当前 API 不依赖
  # C++26，固定到 C++23 以保证独立和主项目预编译库可链接。
  set(CMAKE_CXX_STANDARD 23)
else()
  set(CMAKE_CXX_STANDARD 26)
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include("configure-project")

# 选项定义
option(ICE_BUILD_TESTS "Build the 'testIonCachyEngin' example program." ON)
option(SOURCES_BUILD "Build Deps from sources." OFF)

# 依赖链接偏好独立于宿主项目；作为子项目使用时默认继承宿主项目偏好。
if(DEFINED PROJECT_LINKAGE AND NOT PROJECT_LINKAGE STREQUAL "")
  set(_ice_default_linkage "${PROJECT_LINKAGE}")
else()
  set(_ice_default_linkage "static")
endif()
set(ICE_LINKAGE
    "${_ice_default_linkage}"
    CACHE STRING "Engine dependency linkage preference.")
set_property(CACHE ICE_LINKAGE PROPERTY STRINGS static shared)
string(TOLOWER "${ICE_LINKAGE}" ICE_LINKAGE)
if(NOT ICE_LINKAGE MATCHES "^(static|shared)$")
  message(FATAL_ERROR "ICE_LINKAGE 只能是 static 或 shared。")
endif()
set(ICE_LINKAGE
    "${ICE_LINKAGE}"
    CACHE STRING "Engine dependency linkage preference." FORCE)

# 动态偏好要求依赖库和运行时都使用动态链接，静态偏好保持可独立分发。
if(ICE_LINKAGE STREQUAL "shared")
  set(BUILD_SHARED_LIBS
      ON
      CACHE BOOL "Build shared libraries by default." FORCE)
else()
  set(BUILD_SHARED_LIBS
      OFF
      CACHE BOOL "Build static libraries by default." FORCE)
endif()

# 动态链接偏好不应携带 GNU/Clang 常见静态运行库参数。
if(ICE_LINKAGE STREQUAL "shared" AND NOT MSVC)
  foreach(_ice_link_flag_var IN
          ITEMS CMAKE_EXE_LINKER_FLAGS CMAKE_SHARED_LINKER_FLAGS
                CMAKE_MODULE_LINKER_FLAGS)
    if(DEFINED ${_ice_link_flag_var})
      set(_ice_link_flags "${${_ice_link_flag_var}}")
      foreach(_ice_static_runtime_flag IN ITEMS "-static-libstdc++"
                                                "-static-libgcc" "-static")
        string(REPLACE "${_ice_static_runtime_flag}" "" _ice_link_flags
                       "${_ice_link_flags}")
      endforeach()
      set(${_ice_link_flag_var}
          "${_ice_link_flags}"
          CACHE STRING "Linker flags without static runtime options." FORCE)
    endif()
  endforeach()
endif()

if(MSVC)
  add_compile_options(/utf-8)
  if(ICE_LINKAGE STREQUAL "shared")
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
  else()
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  endif()
endif()

# 包含第三方依赖 3rdpty/CMakeLists.txt 将负责创建依赖库
add_subdirectory(3rdpty)

# 包含源码目录 真正的库和可执行文件定义在 src/CMakeLists.txt 中
add_subdirectory(src)
