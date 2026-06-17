# 3rdpty/sources/cmake/Buildfftw.cmake

include(ExternalProject)

set(FFTW_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/../fftw")
set(FFTW_BINARY_DIR "${CMAKE_BINARY_DIR}/3rdpty/fftw_bld")
set(FFTW_INSTALL_DIR "${CMAKE_BINARY_DIR}/3rdpty/fftw_inst")

if(MSVC)
    set(FFTW_STATIC_LIBRARY "${FFTW_INSTALL_DIR}/lib/fftw3.lib")
else()
    set(FFTW_STATIC_LIBRARY "${FFTW_INSTALL_DIR}/lib/libfftw3.a")
endif()

# Use ExternalProject to build fftw to avoid modifying its source or dealing with its complex CMakeLists.txt directly
# Pass CMAKE_POLICY_VERSION_MINIMUM=3.5 to bypass the fatal error for old cmake_minimum_required
# Force CMAKE_INSTALL_LIBDIR to "lib" to avoid "lib64" on some Linux distros
ExternalProject_Add(fftw_project
    SOURCE_DIR "${FFTW_SOURCE_DIR}"
    BINARY_DIR "${FFTW_BINARY_DIR}"
    INSTALL_DIR "${FFTW_INSTALL_DIR}"
    UPDATE_COMMAND ""
    CMAKE_ARGS
        -DCMAKE_INSTALL_PREFIX=${FFTW_INSTALL_DIR}
        -DCMAKE_INSTALL_LIBDIR=lib
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5
        -DBUILD_SHARED_LIBS=OFF
        -DBUILD_TESTS=OFF
        -DENABLE_THREADS=ON
        -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
        -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON
        -DCMAKE_INSTALL_MESSAGE=NEVER
    INSTALL_COMMAND ${CMAKE_COMMAND} --build . --target install
    BUILD_BYPRODUCTS "${FFTW_STATIC_LIBRARY}"
)

# Pre-create include directory to avoid CMake error during configure
file(MAKE_DIRECTORY "${FFTW_INSTALL_DIR}/include")

add_library(3rd_fftw3 INTERFACE)
add_dependencies(3rd_fftw3 fftw_project)

target_include_directories(3rd_fftw3 INTERFACE "${FFTW_INSTALL_DIR}/include")

target_link_libraries(3rd_fftw3 INTERFACE "${FFTW_STATIC_LIBRARY}")
