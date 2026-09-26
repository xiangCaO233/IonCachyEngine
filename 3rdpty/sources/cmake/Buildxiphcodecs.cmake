# 为 FFmpeg 构建静态 Ogg、Vorbis 和 Opus 编码依赖。 三个上游项目只读取 Git 子模块源码，安装到当前构建树的私有前缀。
# 使用私有前缀使源码模式完全不依赖宿主机的 codec 开发包。 FFmpeg 的 pkg-config 探针也只读取这里生成的 .pc 元数据。
include(ExternalProject)

# 外部项目独立编译，但安装根必须一致，Vorbis 才能找到 Ogg 的配置包。 预编译发布脚本从此目录取归档；变更布局须同步更新发布清单。
set(ICE_XIPH_INSTALL_DIR "${CMAKE_BINARY_DIR}/3rdpty/xiph_inst")
# FFmpeg 编译时只能使用与外部项目同一安装根的公共头。
set(ICE_XIPH_INCLUDE_DIR "${ICE_XIPH_INSTALL_DIR}/include")
# 静态链接探针使用该目录，不能依赖系统库搜索路径。
set(ICE_XIPH_LIBRARY_DIR "${ICE_XIPH_INSTALL_DIR}/lib")
# Vorbis 和 Opus 的能力探测依赖构建后安装的 .pc 文件。
set(ICE_XIPH_PKGCONFIG_DIR "${ICE_XIPH_LIBRARY_DIR}/pkgconfig")

# Windows 静态发布目标使用 /MT 或 /MTd，不能混入上游默认的动态 CRT。
set(ICE_XIPH_MSVC_RUNTIME_LIBRARY "${CMAKE_MSVC_RUNTIME_LIBRARY}")
if(MSVC AND NOT ICE_XIPH_MSVC_RUNTIME_LIBRARY)
  set(ICE_XIPH_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
# 上游 Opus 的 MSVC SIMD 源未携带 clang-cl 所需逐文件目标特性标志。
set(ICE_XIPH_OPUS_FLAGS "")
if(MSVC)
  set(ICE_XIPH_OPUS_FLAGS -DOPUS_DISABLE_INTRINSICS=ON)
endif()

# 每个外部项目必须使用同一目标工具链与配置，避免静态归档混入宿主 ABI。 CMAKE_SYSTEM_* 固定目标平台；只传编译器路径不足以确定交叉编译目标。
# 编译器 target 与 sysroot 共同约束头文件和目标 ABI，不能漏传其一。 安装库目录固定为 lib，避免某些发行版的默认 lib64 与
# FFmpeg 搜索路径分离。 同一配置的三个库使用一致的运行库选择，尤其是 Windows Debug CRT。 归档采用
# PIC，允许随后静态链接到共享模块。
set(ICE_XIPH_CMAKE_ARGS
    # 每个 ExternalProject 都独立配置，安装前缀不能只在父项目设置。
    -DCMAKE_INSTALL_PREFIX=${ICE_XIPH_INSTALL_DIR}
    -DCMAKE_INSTALL_LIBDIR=lib
    # CMake 的 Debug 与 RelWithDebInfo 归档必须落入各自的发布目录。
    -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
    # 目标系统名影响上游的平台分支和产物后缀。
    -DCMAKE_SYSTEM_NAME=${CMAKE_SYSTEM_NAME}
    -DCMAKE_SYSTEM_PROCESSOR=${CMAKE_SYSTEM_PROCESSOR}
    # 使用当前构建选中的 C 编译器，不让上游另行探测宿主默认编译器。
    -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
    # Vorbis 的 CMake 项目启用 C++ 探针，必须传同目标工具链的 C++ 编译器。
    -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}
    -DCMAKE_C_COMPILER_TARGET=${CMAKE_C_COMPILER_TARGET}
    -DCMAKE_CXX_COMPILER_TARGET=${CMAKE_CXX_COMPILER_TARGET}
    -DCMAKE_SYSROOT=${CMAKE_SYSROOT}
    # macOS 的 SDK、架构和部署下限必须与 FFmpeg 及顶层应用保持一致。
    -DCMAKE_OSX_SYSROOT=${CMAKE_OSX_SYSROOT}
    -DCMAKE_OSX_ARCHITECTURES=${CMAKE_OSX_ARCHITECTURES}
    -DCMAKE_OSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET}
    # 与编译器匹配的归档器决定静态库格式及链接兼容性。
    -DCMAKE_AR=${CMAKE_AR}
    -DCMAKE_RANLIB=${CMAKE_RANLIB}
    # Windows 资源编译器沿用目标工具链，不能由宿主默认值替代。
    -DCMAKE_RC_COMPILER=${CMAKE_RC_COMPILER}
    -DCMAKE_MSVC_RUNTIME_LIBRARY=${ICE_XIPH_MSVC_RUNTIME_LIBRARY}
    # 老版上游 CMakeLists 仍需显式启用 CMP0091 才能使用静态运行库属性。
    -DCMAKE_POLICY_DEFAULT_CMP0091=NEW
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON
    # libvorbis 1.3.7 的 CMake 最低版本仍是 2.8；新 CMake 需显式选兼容策略。 兼容策略只作用于该外部项目，不改变主工程的
    # CMake policy。
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5
    # 外部 FFmpeg 是静态包，混入动态 Xiph 库会破坏独立部署。
    -DBUILD_SHARED_LIBS=OFF
    -DBUILD_TESTING=OFF)
# 交叉工具链携带 SDK 头库与链接标志；只传编译器路径无法完成 CMake 探针。
if(CMAKE_TOOLCHAIN_FILE)
  list(APPEND ICE_XIPH_CMAKE_ARGS
       "-DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}")
endif()

# 交叉 MSVC 使用 .lib；其余目标平台使用带 lib 前缀的 .a。 路径变量同时供 ExternalProject byproduct
# 与上层链接依赖使用。 byproduct 必须是实际产物，避免 Ninja 在首次构建时误判依赖不存在。
if(MSVC)
  # Windows 静态 FFmpeg 的构建包装器使用无 lib 前缀的导入名称。
  set(ICE_XIPH_LIB_PREFIX "")
  set(ICE_XIPH_LIB_SUFFIX ".lib")
else()
  # Linux 和 macOS 使用通常的 ar 归档命名。
  set(ICE_XIPH_LIB_PREFIX "lib")
  set(ICE_XIPH_LIB_SUFFIX ".a")
endif()
set(ICE_OGG_STATIC_LIBRARY
    "${ICE_XIPH_LIBRARY_DIR}/${ICE_XIPH_LIB_PREFIX}ogg${ICE_XIPH_LIB_SUFFIX}")
set(ICE_VORBIS_STATIC_LIBRARY
    "${ICE_XIPH_LIBRARY_DIR}/${ICE_XIPH_LIB_PREFIX}vorbis${ICE_XIPH_LIB_SUFFIX}"
)
set(ICE_VORBISENC_STATIC_LIBRARY
    "${ICE_XIPH_LIBRARY_DIR}/${ICE_XIPH_LIB_PREFIX}vorbisenc${ICE_XIPH_LIB_SUFFIX}"
)
set(ICE_OPUS_STATIC_LIBRARY
    "${ICE_XIPH_LIBRARY_DIR}/${ICE_XIPH_LIB_PREFIX}opus${ICE_XIPH_LIB_SUFFIX}")

# Vorbis 的 CMake 包查询需要先安装 Ogg；Opus 串行安装避免写入同一前缀时竞争。 上游源码是子模块，禁用 ExternalProject
# 的更新步骤以保持检出的版本。 不把这些目标直接安装进系统：只有打包脚本可以发布已经验证的归档。
ExternalProject_Add(
  ogg_project
  # 固定源码路径，防止 ExternalProject 尝试重新下载上游版本。
  SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/../ogg"
  BINARY_DIR "${CMAKE_BINARY_DIR}/3rdpty/ogg_bld"
  UPDATE_COMMAND ""
  CMAKE_ARGS ${ICE_XIPH_CMAKE_ARGS} -DINSTALL_DOCS=OFF
  BUILD_BYPRODUCTS "${ICE_OGG_STATIC_LIBRARY}")
# Vorbis 的编码入口在 vorbisenc，解码和公用算法在 vorbis，两个归档都要保留。 CMAKE_PREFIX_PATH 指向 Ogg
# 安装根，避免不慎链接宿主机另一版本的 Ogg。
ExternalProject_Add(
  vorbis_project
  SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/../vorbis"
  BINARY_DIR "${CMAKE_BINARY_DIR}/3rdpty/vorbis_bld"
  UPDATE_COMMAND ""
  # Vorbis 配置依赖 Ogg 安装完毕，不只是编译顺序依赖。
  DEPENDS ogg_project
  CMAKE_ARGS ${ICE_XIPH_CMAKE_ARGS} -DCMAKE_PREFIX_PATH=${ICE_XIPH_INSTALL_DIR}
  BUILD_BYPRODUCTS "${ICE_VORBIS_STATIC_LIBRARY}"
                   "${ICE_VORBISENC_STATIC_LIBRARY}")
# Opus 不依赖 Vorbis 符号；这里串行化是为了保护共享安装前缀。 关闭上游演示程序和测试，预编译包只发布 FFmpeg 所需的静态库。
ExternalProject_Add(
  opus_project
  SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/../opus"
  BINARY_DIR "${CMAKE_BINARY_DIR}/3rdpty/opus_bld"
  UPDATE_COMMAND ""
  # 单安装根中的头和 pkg-config 元数据不能被并发 install 覆盖。
  DEPENDS vorbis_project
  CMAKE_ARGS ${ICE_XIPH_CMAKE_ARGS} -DOPUS_BUILD_TESTING=OFF
             -DOPUS_BUILD_PROGRAMS=OFF -DOPUS_STATIC_RUNTIME=ON
             ${ICE_XIPH_OPUS_FLAGS}
  BUILD_BYPRODUCTS "${ICE_OPUS_STATIC_LIBRARY}")
