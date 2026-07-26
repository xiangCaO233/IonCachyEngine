# 3rdpty/sources/cmake/Buildlame.cmake

include(ExternalProject)
include(ProcessorCount)

ProcessorCount(ICE_LAME_PROCESSOR_COUNT)
if(ICE_LAME_PROCESSOR_COUNT EQUAL 0)
  set(ICE_LAME_PROCESSOR_COUNT 1)
endif()

set(ICE_LAME_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/../lame")
set(ICE_LAME_BINARY_DIR "${CMAKE_BINARY_DIR}/3rdpty/lame_bld")
set(ICE_LAME_INSTALL_DIR "${CMAKE_BINARY_DIR}/3rdpty/lame_inst")
set(ICE_LAME_INCLUDE_DIR "${ICE_LAME_INSTALL_DIR}/include")
set(ICE_LAME_LIBRARY_DIR "${ICE_LAME_INSTALL_DIR}/lib")
set(ICE_LAME_SOURCE_STAMP "${ICE_LAME_INSTALL_DIR}/.ice_lame_sources.stamp")
set(ICE_LAME_SOURCE_READY_TEST
    "test -f '${ICE_LAME_SOURCE_STAMP}' && ! /usr/bin/find '${ICE_LAME_SOURCE_DIR}' -type f -newer '${ICE_LAME_SOURCE_STAMP}' ! -path '*/.git/*' -print -quit | /usr/bin/grep -q ."
)

if(MSVC)
  set(ICE_LAME_STATIC_LIBRARY "${ICE_LAME_LIBRARY_DIR}/mp3lame.lib")
  if(CMAKE_CROSSCOMPILING)
    set(ICE_LAME_BUILD_LIBRARY "${ICE_LAME_LIBRARY_DIR}/libmp3lame.a")
    set(ICE_LAME_VECTOR_OBJECT
        "${ICE_LAME_BINARY_DIR}/libmp3lame/vector/xmm_quantize_sub.obj")
  else()
    set(ICE_LAME_BUILD_LIBRARY "${ICE_LAME_STATIC_LIBRARY}")
  endif()
else()
  set(ICE_LAME_STATIC_LIBRARY "${ICE_LAME_LIBRARY_DIR}/libmp3lame.a")
  set(ICE_LAME_BUILD_LIBRARY "${ICE_LAME_STATIC_LIBRARY}")
endif()

string(TOUPPER "${CMAKE_BUILD_TYPE}" ICE_LAME_BUILD_TYPE_UPPER)
set(ICE_LAME_CONFIG_C_FLAGS "${CMAKE_C_FLAGS_${ICE_LAME_BUILD_TYPE_UPPER}}")

# autotools 不继承 CMake 配置型 CFLAGS，这里显式拼入 Debug/RelWithDebInfo 的 -g。
# 预编译静态库必须保留符号，便于下游定位第三方依赖问题。
set(ICE_LAME_C_FLAGS "${CMAKE_C_FLAGS} ${ICE_LAME_CONFIG_C_FLAGS}")
set(ICE_LAME_LINK_FLAGS "${CMAKE_EXE_LINKER_FLAGS}")
if(CMAKE_C_COMPILER_TARGET)
  string(APPEND ICE_LAME_C_FLAGS " --target=${CMAKE_C_COMPILER_TARGET}")
  string(APPEND ICE_LAME_LINK_FLAGS " --target=${CMAKE_C_COMPILER_TARGET}")
endif()
if(CMAKE_SYSROOT)
  string(APPEND ICE_LAME_C_FLAGS " --sysroot=${CMAKE_SYSROOT}")
  string(APPEND ICE_LAME_LINK_FLAGS " --sysroot=${CMAKE_SYSROOT}")
endif()
if(NOT MSVC)
  string(APPEND ICE_LAME_C_FLAGS " -fPIC")
endif()
string(STRIP "${ICE_LAME_C_FLAGS}" ICE_LAME_C_FLAGS)
string(STRIP "${ICE_LAME_LINK_FLAGS}" ICE_LAME_LINK_FLAGS)

set(ICE_LAME_CONFIGURE_SCRIPT "${ICE_LAME_SOURCE_DIR}/configure")
set(ICE_LAME_CC "${CMAKE_C_COMPILER}")
set(ICE_LAME_AR "${CMAKE_AR}")
if(MSVC AND CMAKE_CROSSCOMPILING)
  # LAME 的 Autotools 接口使用 GCC 风格探测参数；包装器只做参数协议适配， 实际目标对象和静态库仍分别由 clang-cl 与
  # llvm-lib 生成。
  set(ICE_LAME_CC
      "${PROJECT_SOURCE_DIR}/cmake/cross/clang-cl-gcc-compatible.sh")
  set(ICE_LAME_AR "${PROJECT_SOURCE_DIR}/cmake/cross/llvm-lib-ar-compatible.sh")
  if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(ICE_LAME_C_FLAGS "/MTd /Z7 /Od -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00")
  else()
    set(ICE_LAME_C_FLAGS "/MT /Z7 /O2 -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00")
  endif()
  set(ICE_LAME_LINK_FLAGS "")
endif()

set(ICE_LAME_CONFIGURE_COMMAND
    ${CMAKE_COMMAND} -E env "CC=${ICE_LAME_CC}" "AR=${ICE_LAME_AR}"
    "RANLIB=${CMAKE_RANLIB}" "NM=${CMAKE_NM}" "STRIP=${CMAKE_STRIP}"
    "CFLAGS=${ICE_LAME_C_FLAGS}" "LDFLAGS=${ICE_LAME_LINK_FLAGS}")

if(WIN32)
  find_program(ICE_LAME_SH_EXECUTABLE NAMES sh.exe sh bash.exe bash)
  if(NOT ICE_LAME_SH_EXECUTABLE)
    message(
      FATAL_ERROR "LAME requires sh.exe or bash.exe to run configure on Windows"
    )
  endif()
  list(APPEND ICE_LAME_CONFIGURE_COMMAND "${ICE_LAME_SH_EXECUTABLE}")
endif()

list(
  APPEND
  ICE_LAME_CONFIGURE_COMMAND
  "${ICE_LAME_CONFIGURE_SCRIPT}"
  --prefix=${ICE_LAME_INSTALL_DIR}
  --libdir=${ICE_LAME_LIBRARY_DIR}
  --includedir=${ICE_LAME_INCLUDE_DIR}
  --disable-shared
  --enable-static
  --with-pic
  --disable-frontend
  --disable-mp3x
  --disable-mp3rtp
  --disable-gtktest
  --disable-decoder
  --disable-libmpg123)

if(MSVC AND CMAKE_CROSSCOMPILING)
  # 使用 MSVC triplet 避免 Autotools 注入 MinGW 兼容代码；目标对象仍由 clang-cl 生成。
  list(APPEND ICE_LAME_CONFIGURE_COMMAND "--host=x86_64-pc-windows")
elseif(CMAKE_CROSSCOMPILING AND MINGW_TOOLCHAIN_PREFIX)
  # autotools 无法只靠 CC 判断交叉目标；显式传入 --host，避免 configure 尝试运行 Windows 测试程序。
  list(APPEND ICE_LAME_CONFIGURE_COMMAND "--host=${MINGW_TOOLCHAIN_PREFIX}")
endif()

set(ICE_LAME_INSTALL_ACTION "make install")
set(ICE_LAME_BUILD_BYPRODUCTS "${ICE_LAME_BUILD_LIBRARY}")
if(MSVC AND CMAKE_CROSSCOMPILING)
  # LAME 3.100 不会把 x86 SIMD 对象安装进主归档；展开后用 llvm-lib 重建，确保 mp3lame.lib
  # 同时包含主实现和向量实现，且不产生嵌套归档。
  string(
    APPEND
    ICE_LAME_INSTALL_ACTION
    " && '${PROJECT_SOURCE_DIR}/cmake/cross/merge-msvc-archives.sh' '${ICE_LAME_STATIC_LIBRARY}' '${ICE_LAME_BUILD_LIBRARY}' '${ICE_LAME_VECTOR_OBJECT}'"
  )
  list(APPEND ICE_LAME_BUILD_BYPRODUCTS "${ICE_LAME_VECTOR_OBJECT}"
       "${ICE_LAME_STATIC_LIBRARY}")
endif()

ExternalProject_Add(
  lame_project
  SOURCE_DIR "${ICE_LAME_SOURCE_DIR}"
  BINARY_DIR "${ICE_LAME_BINARY_DIR}"
  INSTALL_DIR "${ICE_LAME_INSTALL_DIR}"
  UPDATE_COMMAND ""
  BUILD_ALWAYS TRUE
  CONFIGURE_COMMAND ${ICE_LAME_CONFIGURE_COMMAND}
  BUILD_COMMAND
    sh -c "${ICE_LAME_SOURCE_READY_TEST} || make -j${ICE_LAME_PROCESSOR_COUNT}"
  INSTALL_COMMAND
    sh -c
    "${ICE_LAME_SOURCE_READY_TEST} || (${ICE_LAME_INSTALL_ACTION} && '${CMAKE_COMMAND}' -E touch '${ICE_LAME_SOURCE_STAMP}')"
  BUILD_BYPRODUCTS ${ICE_LAME_BUILD_BYPRODUCTS} "${ICE_LAME_SOURCE_STAMP}")

file(MAKE_DIRECTORY "${ICE_LAME_INCLUDE_DIR}")
file(MAKE_DIRECTORY "${ICE_LAME_LIBRARY_DIR}")

add_library(3rd_lame INTERFACE)
add_dependencies(3rd_lame lame_project)
target_include_directories(3rd_lame INTERFACE "${ICE_LAME_INCLUDE_DIR}")
target_link_libraries(3rd_lame INTERFACE "${ICE_LAME_STATIC_LIBRARY}")
