include(ExternalProject)

# 定义项目路径配置
set(RUBBERBAND_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../sources/rubberband")
set(RUBBERBAND_BUILD_DIR "${CMAKE_CURRENT_BINARY_DIR}/rubberband_build")
set(RUBBERBAND_INSTALL_DIR "${CMAKE_CURRENT_BINARY_DIR}/rubberband_install")

# 跨平台编译标志
set(EXTRA_C_FLAGS_LIST "")
if(MSVC)

else()
    list(APPEND EXTRA_C_FLAGS_LIST "-fPIC" "-march=native")
endif()

# Meson 用 ' ' 分隔的字符串作为 c_args
string(REPLACE ";" " " EXTRA_C_FLAGS_STR "${EXTRA_C_FLAGS_LIST}")

# 构造 Meson 配置参数
set(MESON_SETUP_ARGS
    --prefix=${RUBBERBAND_INSTALL_DIR}
    --libdir=lib
    -Ddefault_library=static # 只构建静态库
    -Dtests=disabled # 禁用测试
    -Dcmdline=disabled # 禁用命令行工具（这将避免寻找 sndfile）
    -Dvamp=disabled # 禁用 Vamp 插件
    -Dladspa=disabled # 禁用 LADSPA 插件
    -Dlv2=disabled # 禁用 LV2 插件
    -Djni=disabled # 禁用 JNI
    -Dfft=builtin # 使用内置 FFT
    -Dresampler=speex # 使用内置 Speex 重采样器
    # 注入跨平台优化和 PIC 标志
    -Dc_args=${EXTRA_C_FLAGS_STR}
    -Dcpp_args=${EXTRA_C_FLAGS_STR})

# 构建项目
ExternalProject_Add(
  rubberband_project
  SOURCE_DIR ${RUBBERBAND_SOURCE_DIR}
  # Meson 需要独立的构建目录
  BINARY_DIR ${RUBBERBAND_BUILD_DIR}
  # 定义配置、构建、安装命令
  CONFIGURE_COMMAND meson setup ${MESON_SETUP_ARGS} ${RUBBERBAND_BUILD_DIR}
                    ${RUBBERBAND_SOURCE_DIR}
  BUILD_COMMAND meson compile -C ${RUBBERBAND_BUILD_DIR}
  INSTALL_COMMAND meson install -C ${RUBBERBAND_BUILD_DIR}
  # 定义产物，帮助 Ninja 等构建系统理解依赖关系
  BUILD_BYPRODUCTS ${RUBBERBAND_INSTALL_DIR}/lib/librubberband.a)

# 封装为 CMake 接口库
file(MAKE_DIRECTORY ${RUBBERBAND_INSTALL_DIR}/include)
add_library(3rd_rubberband INTERFACE)
add_dependencies(3rd_rubberband rubberband_project)

target_include_directories(3rd_rubberband
                           INTERFACE ${RUBBERBAND_INSTALL_DIR}/include)

# Windows 下静态库名可能不同，但 Meson 通常会统一为 librubberband.a
target_link_libraries(3rd_rubberband
                      INTERFACE ${RUBBERBAND_INSTALL_DIR}/lib/librubberband.a)

# Rubberband 静态库可能需要链接 C++ 标准库和数学库
if(NOT WIN32)
    target_link_libraries(3rd_rubberband INTERFACE m)
    # 在某些系统上可能需要显式链接 stdc++ target_link_libraries(3rd_rubberband INTERFACE stdc++)
endif()
