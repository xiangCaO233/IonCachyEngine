# CMakeLists.txt

cmake_minimum_required(VERSION 3.22)

# 同时启用 C 是为源码依赖提供编译器信息；引擎业务实现使用 C++。
project(IonCachyEngine LANGUAGES CXX C)

# 独立项目入口统一语言标准和编译命令导出，供引擎源码及自测目标共用。 作为宿主子项目时也保留引擎自己的标准选择，不要求业务 API 使用 C++26。
# 全局设置
if(APPLE
   OR MSVC
   OR (WIN32 AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU"))
  # MinGW GCC 16 的 gnu++26 与静态 libstdc++ 链接会重复定义 stdexcept 符号。 引擎当前 API 不依赖
  # C++26，固定到 C++23 以保证独立和主项目预编译库可链接。
  set(CMAKE_CXX_STANDARD 23)
else()
  # 非兼容性分支保留引擎既有标准选择，由编译器能力检查决定是否可用。
  set(CMAKE_CXX_STANDARD 26)
endif()
# 不允许编译器静默降级标准，否则独立验证可能与实际宿主构建不一致。
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# 导出命令供逐翻译单元诊断使用，不改变最终库的链接来源。
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 引擎本地配置模块只影响自身约定，不依赖宿主项目的配置脚本。
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include("configure-project")

# ICE_BUILD_TESTS 同时控制示例程序和实时安全回归测试。 SOURCES_BUILD 只决定外部依赖来源，引擎自身在此工程始终从源码编译。
# 关闭依赖源码构建时，缺少预编译包必须报错，不能自动回退。 选项定义
option(ICE_BUILD_TESTS "Build the 'testIonCachyEngin' example program." ON)
option(SOURCES_BUILD "Build Deps from sources." OFF)
if(ICE_BUILD_TESTS)
  # 独立构建引擎时启用 CTest；作为主项目子目录时沿用主项目测试入口。 根级启用后，CTest 才能遍历 src 中登记的实时安全用例。
  enable_testing()
endif()

# 依赖链接偏好独立于宿主项目；作为子项目使用时默认继承宿主项目偏好。 只在宿主显式提供非空偏好时继承，否则独立构建采用静态默认值。
if(DEFINED PROJECT_LINKAGE AND NOT PROJECT_LINKAGE STREQUAL "")
  set(_ice_default_linkage "${PROJECT_LINKAGE}")
else()
  # 独立使用无需先定义宿主变量即可选择仓库提供的静态包布局。
  set(_ice_default_linkage "static")
endif()
# 缓存已有值优先于本次推导的默认值，保留使用者显式选择。
set(ICE_LINKAGE
    "${_ice_default_linkage}"
    CACHE STRING "Engine dependency linkage preference.")
# 缓存候选用于配置界面展示，下面的正则检查才是实际合法性约束。
set_property(CACHE ICE_LINKAGE PROPERTY STRINGS static shared)
# 先归一化大小写，再将合法结果写回缓存，保证各查找脚本看到一致偏好。
string(TOLOWER "${ICE_LINKAGE}" ICE_LINKAGE)
if(NOT ICE_LINKAGE MATCHES "^(static|shared)$")
  # 无法识别的偏好不能继续猜测包路径，否则可能错误混用运行库。
  message(FATAL_ERROR "ICE_LINKAGE 只能是 static 或 shared。")
endif()
set(ICE_LINKAGE
    "${ICE_LINKAGE}"
    CACHE STRING "Engine dependency linkage preference." FORCE)

# 链接偏好控制目标类型与运行库选择，不能只更换最终库文件后缀。 显式写回 BUILD_SHARED_LIBS，防止复用构建目录时残留上一次的选择。
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
  # 分别处理可执行文件、共享库和模块的链接选项，避免只清理一种目标。
  foreach(_ice_link_flag_var IN
          ITEMS CMAKE_EXE_LINKER_FLAGS CMAKE_SHARED_LINKER_FLAGS
                CMAKE_MODULE_LINKER_FLAGS)
    if(DEFINED ${_ice_link_flag_var})
      set(_ice_link_flags "${${_ice_link_flag_var}}")
      # 这里只清理已列出的常见静态运行库选项，不重写其他用户链接参数。
      foreach(_ice_static_runtime_flag IN ITEMS "-static-libstdc++"
                                                "-static-libgcc" "-static")
        string(REPLACE "${_ice_static_runtime_flag}" "" _ice_link_flags
                       "${_ice_link_flags}")
      endforeach()
      # 更新缓存，保证下次配置和后续创建的目标继续使用清理后的选项。
      set(${_ice_link_flag_var}
          "${_ice_link_flags}"
          CACHE STRING "Linker flags without static runtime options." FORCE)
    endif()
  endforeach()
endif()

if(MSVC)
  # 统一源文本编码；运行库模式还必须与所选预编译依赖的 ABI 匹配。
  add_compile_options(/utf-8)
  # Debug 使用调试 CRT，其余配置使用发布 CRT，不跨配置混用二进制。
  if(ICE_LINKAGE STREQUAL "shared")
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
  else()
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  endif()
endif()

# 先创建依赖目标再进入 src，使业务目标只链接稳定 target 名而非裸路径。 包含第三方依赖 3rdpty/CMakeLists.txt
# 将负责创建依赖库
add_subdirectory(3rdpty)

# 测试与库目标归 src 管理；顶层不重复收集源码或创建替代别名。 包含源码目录 真正的库和可执行文件定义在 src/CMakeLists.txt 中
add_subdirectory(src)
