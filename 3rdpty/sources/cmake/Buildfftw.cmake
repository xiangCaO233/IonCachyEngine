# 3rdpty/sources/cmake/Buildfftw.cmake

include(ExternalProject)

set(FFTW_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/../fftw")
set(FFTW_BINARY_DIR "${CMAKE_BINARY_DIR}/3rdpty/fftw_bld")
set(FFTW_INSTALL_DIR "${CMAKE_BINARY_DIR}/3rdpty/fftw_inst")
set(FFTW_SOURCE_STAMP "${FFTW_INSTALL_DIR}/.ice_fftw_sources.stamp")
set(FFTW_SOURCE_READY_TEST
    "test -f '${FFTW_SOURCE_STAMP}' && ! /usr/bin/find '${FFTW_SOURCE_DIR}' -type f -newer '${FFTW_SOURCE_STAMP}' ! -path '*/.git/*' -print -quit | /usr/bin/grep -q ."
)

# FFTW 是 Rubber Band 的底层依赖，shared 预编译包中必须提供 DLL 和导入库。
set(FFTW_BUILD_SHARED OFF)
if(ICE_LINKAGE STREQUAL "shared")
  set(FFTW_BUILD_SHARED ON)
endif()
set(FFTW_RUNTIME_LIBRARY "")
if(MSVC)
  set(FFTW_LIBRARY "${FFTW_INSTALL_DIR}/lib/fftw3.lib")
  if(FFTW_BUILD_SHARED)
    set(FFTW_RUNTIME_LIBRARY "${FFTW_INSTALL_DIR}/bin/fftw3.dll")
  endif()
elseif(FFTW_BUILD_SHARED)
  set(FFTW_LIBRARY
      "${FFTW_INSTALL_DIR}/lib/${CMAKE_SHARED_LIBRARY_PREFIX}fftw3${CMAKE_SHARED_LIBRARY_SUFFIX}"
  )
else()
  set(FFTW_LIBRARY "${FFTW_INSTALL_DIR}/lib/libfftw3.a")
endif()

set(FFTW_ENABLE_THREADS ON)
if(MSVC)
  # MSVC 原生环境没有 pthread.h，禁用 FFTW threads 子库以避免误启用 pthread 后端。
  set(FFTW_ENABLE_THREADS OFF)
endif()

set(FFTW_C_FLAGS "${CMAKE_C_FLAGS}")
if(MSVC)
  string(APPEND FFTW_C_FLAGS " /wd4244")
  if(CMAKE_MSVC_RUNTIME_LIBRARY)
    set(FFTW_MSVC_RUNTIME_LIBRARY "${CMAKE_MSVC_RUNTIME_LIBRARY}")
  elseif(CMAKE_BUILD_TYPE MATCHES Debug)
    set(FFTW_MSVC_RUNTIME_LIBRARY "MultiThreadedDebug")
  else()
    set(FFTW_MSVC_RUNTIME_LIBRARY "MultiThreaded")
  endif()
  if(FFTW_MSVC_RUNTIME_LIBRARY MATCHES "DLL")
    set(FFTW_MSVC_RUNTIME_FLAG_RELEASE "/MD")
    set(FFTW_MSVC_RUNTIME_FLAG_DEBUG "/MDd")
  else()
    set(FFTW_MSVC_RUNTIME_FLAG_RELEASE "/MT")
    set(FFTW_MSVC_RUNTIME_FLAG_DEBUG "/MTd")
  endif()

  # FFTW 的旧 CMakeLists 不识别 CMAKE_MSVC_RUNTIME_LIBRARY，必须覆盖各配置 flags 才能确保预编译布局中的
  # CRT 与链接偏好一致。
  set(FFTW_MSVC_RUNTIME_ARGS
      "-DCMAKE_C_FLAGS_DEBUG=${FFTW_MSVC_RUNTIME_FLAG_DEBUG} /Zi /Ob0 /Od /RTC1"
      "-DCMAKE_C_FLAGS_RELEASE=${FFTW_MSVC_RUNTIME_FLAG_RELEASE} /O2 /Ob2 /DNDEBUG"
      "-DCMAKE_C_FLAGS_RELWITHDEBINFO=${FFTW_MSVC_RUNTIME_FLAG_RELEASE} /Zi /O2 /Ob1 /DNDEBUG"
      "-DCMAKE_C_FLAGS_MINSIZEREL=${FFTW_MSVC_RUNTIME_FLAG_RELEASE} /O1 /Ob1 /DNDEBUG"
  )
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
  # FFTW 的旧 CMake 工程不会自动继承外层交叉工具链；这里显式传入目标系统和 try-compile 类型，避免宿主 Linux 链接参数污染
  # MinGW 构建。
  CMAKE_ARGS -DCMAKE_INSTALL_PREFIX=${FFTW_INSTALL_DIR}
             -DCMAKE_INSTALL_LIBDIR=lib
             -DCMAKE_POLICY_VERSION_MINIMUM=3.5
             -DBUILD_SHARED_LIBS=${FFTW_BUILD_SHARED}
             -DBUILD_TESTS=OFF
             -DENABLE_THREADS=${FFTW_ENABLE_THREADS}
             -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
             -DCMAKE_SYSTEM_NAME=${CMAKE_SYSTEM_NAME}
             -DCMAKE_SYSTEM_PROCESSOR=${CMAKE_SYSTEM_PROCESSOR}
             -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
             -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}
             -DCMAKE_C_COMPILER_TARGET=${CMAKE_C_COMPILER_TARGET}
             -DCMAKE_CXX_COMPILER_TARGET=${CMAKE_CXX_COMPILER_TARGET}
             -DCMAKE_SYSROOT=${CMAKE_SYSROOT}
             -DCMAKE_AR=${CMAKE_AR}
             -DCMAKE_RANLIB=${CMAKE_RANLIB}
             -DCMAKE_RC_COMPILER=${CMAKE_RC_COMPILER}
             -DCMAKE_NM=${CMAKE_NM}
             -DCMAKE_STRIP=${CMAKE_STRIP}
             -DCMAKE_OBJCOPY=${CMAKE_OBJCOPY}
             -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY
             "-DCMAKE_C_FLAGS=${FFTW_C_FLAGS}"
             ${FFTW_MSVC_RUNTIME_ARGS}
             -DCMAKE_POSITION_INDEPENDENT_CODE=ON
             -DCMAKE_INSTALL_MESSAGE=NEVER
  BUILD_COMMAND sh -c
                "${FFTW_SOURCE_READY_TEST} || '${CMAKE_COMMAND}' --build ."
  INSTALL_COMMAND
    sh -c
    "${FFTW_SOURCE_READY_TEST} || ('${CMAKE_COMMAND}' --build . --target install && '${CMAKE_COMMAND}' -E touch '${FFTW_SOURCE_STAMP}')"
  BUILD_BYPRODUCTS "${FFTW_LIBRARY}" ${FFTW_RUNTIME_LIBRARY}
                   "${FFTW_SOURCE_STAMP}")

# 预创建 include 目录，避免 CMake 配置阶段报错。
file(MAKE_DIRECTORY "${FFTW_INSTALL_DIR}/include")

add_library(3rd_fftw3 INTERFACE)
add_dependencies(3rd_fftw3 fftw_project)

target_include_directories(3rd_fftw3 INTERFACE "${FFTW_INSTALL_DIR}/include")

target_link_libraries(3rd_fftw3 INTERFACE "${FFTW_LIBRARY}")
