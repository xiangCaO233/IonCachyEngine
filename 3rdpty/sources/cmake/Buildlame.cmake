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
else()
  set(ICE_LAME_STATIC_LIBRARY "${ICE_LAME_LIBRARY_DIR}/libmp3lame.a")
endif()

string(TOUPPER "${CMAKE_BUILD_TYPE}" ICE_LAME_BUILD_TYPE_UPPER)
set(ICE_LAME_CONFIG_C_FLAGS "${CMAKE_C_FLAGS_${ICE_LAME_BUILD_TYPE_UPPER}}")

# autotools 不继承 CMake 配置型 CFLAGS，这里显式拼入 Debug/RelWithDebInfo 的 -g。
# 预编译静态库必须保留符号，便于下游定位第三方依赖问题。
set(ICE_LAME_C_FLAGS "${CMAKE_C_FLAGS} ${ICE_LAME_CONFIG_C_FLAGS}")
if(NOT MSVC)
  string(APPEND ICE_LAME_C_FLAGS " -fPIC")
endif()

set(ICE_LAME_CONFIGURE_SCRIPT "${ICE_LAME_SOURCE_DIR}/configure")
set(ICE_LAME_CONFIGURE_COMMAND
    ${CMAKE_COMMAND} -E env "CC=${CMAKE_C_COMPILER}" "AR=${CMAKE_AR}"
    "RANLIB=${CMAKE_RANLIB}" "CFLAGS=${ICE_LAME_C_FLAGS}")

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
    "${ICE_LAME_SOURCE_READY_TEST} || (make install && '${CMAKE_COMMAND}' -E touch '${ICE_LAME_SOURCE_STAMP}')"
  BUILD_BYPRODUCTS "${ICE_LAME_STATIC_LIBRARY}" "${ICE_LAME_SOURCE_STAMP}")

file(MAKE_DIRECTORY "${ICE_LAME_INCLUDE_DIR}")
file(MAKE_DIRECTORY "${ICE_LAME_LIBRARY_DIR}")

add_library(3rd_lame INTERFACE)
add_dependencies(3rd_lame lame_project)
target_include_directories(3rd_lame INTERFACE "${ICE_LAME_INCLUDE_DIR}")
target_link_libraries(3rd_lame INTERFACE "${ICE_LAME_STATIC_LIBRARY}")
