# 3rdpty/sources/cmake/Buildfftw.cmake

include(ExternalProject)

set(FFTW_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/../fftw")
set(FFTW_BINARY_DIR "${CMAKE_BINARY_DIR}/3rdpty/fftw_bld")
set(FFTW_INSTALL_DIR "${CMAKE_BINARY_DIR}/3rdpty/fftw_inst")
set(FFTW_SOURCE_STAMP "${FFTW_INSTALL_DIR}/.mmm_fftw_sources.stamp")
set(FFTW_SOURCE_READY_TEST
    "test -f '${FFTW_SOURCE_STAMP}' && ! find '${FFTW_SOURCE_DIR}' -type f -newer '${FFTW_SOURCE_STAMP}' ! -path '*/.git/*' -print -quit | grep -q ."
)

if(MSVC)
  set(FFTW_STATIC_LIBRARY "${FFTW_INSTALL_DIR}/lib/fftw3.lib")
else()
  set(FFTW_STATIC_LIBRARY "${FFTW_INSTALL_DIR}/lib/libfftw3.a")
endif()

set(FFTW_C_FLAGS "${CMAKE_C_FLAGS}")
if(MSVC)
  string(APPEND FFTW_C_FLAGS " /wd4244")
endif()
string(STRIP "${FFTW_C_FLAGS}" FFTW_C_FLAGS)

# 使用 ExternalProject 构建 FFTW，避免修改源码或直接处理其复杂的 CMakeLists。 传入
# CMAKE_POLICY_VERSION_MINIMUM=3.5，绕过旧 cmake_minimum_required 带来的致命错误。 强制
# CMAKE_INSTALL_LIBDIR 为 "lib"，避免部分 Linux 发行版使用 "lib64"。
ExternalProject_Add(
  fftw_project
  SOURCE_DIR "${FFTW_SOURCE_DIR}"
  BINARY_DIR "${FFTW_BINARY_DIR}"
  INSTALL_DIR "${FFTW_INSTALL_DIR}"
  UPDATE_COMMAND ""
  BUILD_ALWAYS TRUE
  CMAKE_ARGS -DCMAKE_INSTALL_PREFIX=${FFTW_INSTALL_DIR}
             -DCMAKE_INSTALL_LIBDIR=lib
             -DCMAKE_POLICY_VERSION_MINIMUM=3.5
             -DBUILD_SHARED_LIBS=OFF
             -DBUILD_TESTS=OFF
             -DENABLE_THREADS=ON
             -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
             -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
             "-DCMAKE_C_FLAGS=${FFTW_C_FLAGS}"
             -DCMAKE_POSITION_INDEPENDENT_CODE=ON
             -DCMAKE_INSTALL_MESSAGE=NEVER
  BUILD_COMMAND sh -c "${FFTW_SOURCE_READY_TEST} || ${CMAKE_COMMAND} --build ."
  INSTALL_COMMAND
    sh -c
    "${FFTW_SOURCE_READY_TEST} || (${CMAKE_COMMAND} --build . --target install && ${CMAKE_COMMAND} -E touch '${FFTW_SOURCE_STAMP}')"
  BUILD_BYPRODUCTS "${FFTW_STATIC_LIBRARY}" "${FFTW_SOURCE_STAMP}")

# 预创建 include 目录，避免 CMake 配置阶段报错。
file(MAKE_DIRECTORY "${FFTW_INSTALL_DIR}/include")

add_library(3rd_fftw3 INTERFACE)
add_dependencies(3rd_fftw3 fftw_project)

target_include_directories(3rd_fftw3 INTERFACE "${FFTW_INSTALL_DIR}/include")

target_link_libraries(3rd_fftw3 INTERFACE "${FFTW_STATIC_LIBRARY}")
