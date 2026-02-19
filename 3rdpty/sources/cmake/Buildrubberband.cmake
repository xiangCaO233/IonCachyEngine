include(ExternalProject)

# =========================================================================
# 1. 自动映射 CMake 到 Meson 的构建类型
# =========================================================================
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(RB_BUILD_TYPE "debug")
    # Debug 模式下我们不需要 march=native 干扰调试，或者保持开启以确保 DSP 性能
    set(RB_FLAGS "-g")
else()
    set(RB_BUILD_TYPE "release")
    set(RB_FLAGS "-O3 -march=native")
endif()

message(STATUS "Rubberband Build Type: ${RB_BUILD_TYPE}")

# 定义项目路径配置
# 强制使用绝对路径，并显著缩短构建路径深度
# 将构建目录移至 CMAKE_BINARY_DIR (即 cmake-build-xxx 根部)，而不是嵌套在 3rdpty 文件夹深处
get_filename_component(RUBBERBAND_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../sources/rubberband" ABSOLUTE)
set(RUBBERBAND_BUILD_DIR "${CMAKE_BINARY_DIR}/rb_bld")
set(RUBBERBAND_INSTALL_DIR "${CMAKE_BINARY_DIR}/rb_inst")

# =========================================================================
# 3. 构造 Meson 配置参数
# =========================================================================
set(MESON_SETUP_ARGS
        --prefix=${RUBBERBAND_INSTALL_DIR}
        --libdir=lib
        --buildtype=${RB_BUILD_TYPE}   # 核心：映射构建类型
        -Ddefault_library=static
        -Dtests=disabled
        -Dcmdline=disabled
        -Dvamp=disabled
        -Dladspa=disabled
        -Dlv2=disabled
        -Djni=disabled
        -Dfft=fftw
        -Dresampler=libsamplerate
        "-Dc_args=${RB_FLAGS}"
        "-Dcpp_args=${RB_FLAGS}"
)

# 确定库文件产物路径
# MinGW 环境下 Meson 通常生成 librubberband.a
set(RUBBERBAND_STATIC_LIB "${RUBBERBAND_INSTALL_DIR}/lib/librubberband.a")

# 构建外部项目
ExternalProject_Add(
        rubberband_project
        SOURCE_DIR "${RUBBERBAND_SOURCE_DIR}"
        BINARY_DIR "${RUBBERBAND_BUILD_DIR}"

        # 在 CONFIGURE_COMMAND 中显式指定构建目录和源码目录的绝对路径
        # 这会覆盖 Meson 默认生成的脆弱相对路径
        CONFIGURE_COMMAND meson setup ${MESON_SETUP_ARGS} "${RUBBERBAND_BUILD_DIR}" "${RUBBERBAND_SOURCE_DIR}"

        BUILD_COMMAND meson compile -C "${RUBBERBAND_BUILD_DIR}"
        INSTALL_COMMAND meson install -C "${RUBBERBAND_BUILD_DIR}"

        # 定义产物，帮助 Ninja 理解依赖
        BUILD_BYPRODUCTS "${RUBBERBAND_STATIC_LIB}"
)

# 封装为 CMake 接口库
# 预先创建 include 目录防止 CMake 配置阶段因为找不到目录报错
file(MAKE_DIRECTORY "${RUBBERBAND_INSTALL_DIR}/include")

add_library(3rd_rubberband INTERFACE)
add_dependencies(3rd_rubberband rubberband_project)

# 使用绝对路径关联头文件和库
target_include_directories(3rd_rubberband INTERFACE "${RUBBERBAND_INSTALL_DIR}/include")
target_link_libraries(3rd_rubberband 
    INTERFACE
    "${RUBBERBAND_STATIC_LIB}"
)

if(APPLE)
    find_package(FFTW3 REQUIRED)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SAMPLERATE REQUIRED samplerate)
    target_include_directories(3rd_rubberband INTERFACE ${FFTW3_INCLUDE_DIRS})
    target_link_directories(3rd_rubberband INTERFACE "/opt/homebrew/lib")
    target_link_libraries(3rd_rubberband
        INTERFACE
        "${FFTW3_LIBRARY_DIRS}/libfftw3.a"
        "${SAMPLERATE_LIBRARIES}"
    )
endif()

# 系统底层库链接
if(WIN32)
    # MinGW-UCRT64 静态链接通常需要 pthread
    target_link_libraries(3rd_rubberband INTERFACE pthread)
else()
    target_link_libraries(3rd_rubberband INTERFACE m)
    if(NOT APPLE)
        target_link_libraries(3rd_rubberband INTERFACE pthread)
    endif()
endif()

