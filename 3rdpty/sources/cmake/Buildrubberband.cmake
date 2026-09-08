# 3rdpty/sources/cmake/Buildrubberband.cmake

# * 本入口维护 Meson 构建适配与稳定消费接口，不修改 Rubber Band 上游源码。
# * 只由源码依赖模式进入；FFTW 外部项目和 samplerate 目标必须已由前置脚本建立。
include(ExternalProject)
include("${PROJECT_SOURCE_DIR}/cmake/ICEMsvcExternalEnvironment.cmake")

# * 将 CMake 构建类型映射到 Meson。 预编译库的 Debug 与 RelWithDebInfo 必须保留调试信息。
# * 配置名按精确大小写判断，非标准配置不会自动继承 Debug 或含符号发布策略。
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  set(RB_BUILD_TYPE "debug")
  # MSVC 的调试信息交给 Meson buildtype/CRT 配置，不额外附加 GNU 风格 -g。
  if(MSVC)
    set(RB_FLAGS "")
  else()
    set(RB_FLAGS "-g")
  endif()
elseif(CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
  set(RB_BUILD_TYPE "debugoptimized")
  # 发布含符号类型保留优化与诊断信息，不能退化成普通 Meson release。
  if(MSVC)
    set(RB_FLAGS "")
  else()
    set(RB_FLAGS "-O2 -g")
  endif()
else()
  set(RB_BUILD_TYPE "release")
  # MinSizeRel 在此也进入 release，当前没有单独映射 Meson 的体积优化类型。
  if(MSVC)
    set(RB_FLAGS "")
  else()
    set(RB_FLAGS "-O3")
  endif()
endif()

# * 日志展示选定的 Meson 类型，不表示外部项目已经成功配置或保留了实际符号。
message(STATUS "Rubberband Build Type: ${RB_BUILD_TYPE}")

# * 先尝试几个固定 Windows 工具目录，再回退环境搜索；顺序不根据当前 Clang/GCC 自动调整。
# * 找到 meson 可执行文件不校验其版本、Python 运行环境或后续 Ninja 是否可用。
find_program(
  RUBBERBAND_MESON_EXE
  NAMES meson
  PATHS "C:/Program Files/Meson" "C:/msys64/ucrt64/bin" "C:/msys64/clang64/bin"
        "C:/msys64/mingw64/bin"
  NO_DEFAULT_PATH)
if(NOT RUBBERBAND_MESON_EXE)
  find_program(RUBBERBAND_MESON_EXE NAMES meson)
endif()
# * 缺 Meson 在配置期报错，不自动下载安装工具或回退到系统 Rubber Band 库。
if(NOT RUBBERBAND_MESON_EXE)
  message(FATAL_ERROR "构建 Rubber Band 需要 meson，可执行文件未找到。")
endif()

# * 判断 Clang/GCC 的 LTO 参数
# * 这组额外参数只面向非 Apple 的 Clang 发布配置，不是 GCC 的通用 LTO 配置。
# * 独立引擎使用自身的 LTO 开关，嵌入时由调用方传入偏好。
set(RB_LTO_FLAGS "")
if(NOT APPLE
   AND CMAKE_CXX_COMPILER_ID MATCHES "Clang"
   AND NOT ICE_DISABLE_CLANG_LTO
   AND (CMAKE_BUILD_TYPE STREQUAL "Release"
        OR CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo"
        OR CMAKE_BUILD_TYPE STREQUAL "MinSizeRel"))
  # * ThinLTO 的对象与最终链接器能力必须匹配，参数存在不证明依赖包可在别的工具链复用。
  list(APPEND RB_LTO_FLAGS "-flto=thin" "-fsplit-lto-unit")
endif()

# 合并额外编译和链接参数
set(RB_EXTRA_FLAGS_LIST "")
# 列表每次从空重建，避免重复包含时累加上一轮额外参数。
if(RB_LTO_FLAGS)
  list(APPEND RB_EXTRA_FLAGS_LIST ${RB_LTO_FLAGS})
endif()

# * 把参数列表转换为空格串以传入 Meson，不进行额外 shell 转义。
string(REPLACE ";" " " RB_EXTRA_FLAGS "${RB_EXTRA_FLAGS_LIST}")

# * C、C++ 编译参数和链接参数分别存储，避免把编译专属标志自动混入链接探针。
set(C_ARGS_VAL "${RB_FLAGS}")
set(CPP_ARGS_VAL "${RB_FLAGS}")
# 链接串从空值建立，不把调试/优化编译 flags 自动当作链接器参数。
set(C_LINK_ARGS_VAL "")
set(CPP_LINK_ARGS_VAL "")

# * Meson 不理解 CMake 的 CMAKE_<LANG>_COMPILER_TARGET，交叉构建时需要显式把 clang 目标、sysroot
#   与链接运行库参数写入 Meson 参数。
# * 父级通用 flags 会传入外部项目，业务 PGO 必须按目标隔离，不能全目录透传。
# * 本段未逐项继承父级各配置 flags，而是将通用 flags 与上面映射的 RB_FLAGS 合并。
set(RB_TOOLCHAIN_C_FLAGS "${CMAKE_C_FLAGS}")
set(RB_TOOLCHAIN_CPP_FLAGS "${CMAKE_CXX_FLAGS}")
# C 与 C++ 共享通用 EXE 链接 flags，未按库类型选择 SHARED_LINKER_FLAGS。
set(RB_TOOLCHAIN_LINK_FLAGS "${CMAKE_EXE_LINKER_FLAGS}")
# clang-cl 的 Meson sanity 检查经过编译驱动，不能直接接收 lld-link 的 /libpath。 从工具链提取库目录到
# LIB，配置、依赖扫描和实际链接都使用同一环境。 ExternalProject 用占位分隔符传递 Windows 分号列表，保留带空格的 SDK 路径。
set(RB_MSVC_ENV "")
if(MSVC AND CMAKE_CROSSCOMPILING)
  # 链接功能探针通过 clang-cl 驱动执行，不能仅为最终链接配置 c_ld。
  string(APPEND RB_TOOLCHAIN_C_FLAGS " -fuse-ld=lld")
  string(APPEND RB_TOOLCHAIN_CPP_FLAGS " -fuse-ld=lld")
  separate_arguments(_rb_link_flags UNIX_COMMAND "${RB_TOOLCHAIN_LINK_FLAGS}")
  set(RB_TOOLCHAIN_LINK_FLAGS "")
  foreach(_rb_flag IN LISTS _rb_link_flags)
    if(_rb_flag MATCHES "^/[Ll][Ii][Bb][Pp][Aa][Tt][Hh]:(.*)$")
      # 库搜索路径由公共环境 helper 提供，不传入 Meson 链接参数。
    else()
      string(APPEND RB_TOOLCHAIN_LINK_FLAGS " \"${_rb_flag}\"")
    endif()
  endforeach()
  # Meson 的 MSVC 头依赖探针可能不带 c_args，因此同时固化 INCLUDE。
  ice_msvc_external_environment(RB_MSVC_ENV "__ICE_RB_SEPARATOR__")
endif()

# * 本分支由 C target 是否存在控制，C++ 追加的是 CXX target；两者需由工具链一致设置。
# * 显式 triple 约束编译与链接目标，但不能代替 Meson 的 host_machine 描述。
if(CMAKE_C_COMPILER_TARGET)
  string(APPEND RB_TOOLCHAIN_C_FLAGS " --target=${CMAKE_C_COMPILER_TARGET}")
  string(APPEND RB_TOOLCHAIN_CPP_FLAGS " --target=${CMAKE_CXX_COMPILER_TARGET}")
  string(APPEND RB_TOOLCHAIN_LINK_FLAGS " --target=${CMAKE_C_COMPILER_TARGET}")
endif()
# * sysroot 同时进入编译与链接探针参数，不能只限制头文件而继续链接宿主库。
if(CMAKE_SYSROOT)
  string(APPEND RB_TOOLCHAIN_C_FLAGS " --sysroot=${CMAKE_SYSROOT}")
  string(APPEND RB_TOOLCHAIN_CPP_FLAGS " --sysroot=${CMAKE_SYSROOT}")
  string(APPEND RB_TOOLCHAIN_LINK_FLAGS " --sysroot=${CMAKE_SYSROOT}")
endif()
# * 只去掉参数串首尾空白，不解析引号、转义或冲突选项。
string(STRIP "${RB_TOOLCHAIN_C_FLAGS}" RB_TOOLCHAIN_C_FLAGS)
string(STRIP "${RB_TOOLCHAIN_CPP_FLAGS}" RB_TOOLCHAIN_CPP_FLAGS)
string(STRIP "${RB_TOOLCHAIN_LINK_FLAGS}" RB_TOOLCHAIN_LINK_FLAGS)

if(NOT "${RB_TOOLCHAIN_C_FLAGS}" STREQUAL "")
  set(C_ARGS_VAL "${RB_TOOLCHAIN_C_FLAGS} ${C_ARGS_VAL}")
endif()
if(NOT "${RB_TOOLCHAIN_CPP_FLAGS}" STREQUAL "")
  set(CPP_ARGS_VAL "${RB_TOOLCHAIN_CPP_FLAGS} ${CPP_ARGS_VAL}")
  # 参数按父级在前、本脚本映射在后排列，冲突项最终效果取决于驱动解析顺序。
endif()
# * 先复制通用链接参数；下方存在额外 LTO 参数时当前实现会覆盖而不是继续追加。
if(NOT "${RB_TOOLCHAIN_LINK_FLAGS}" STREQUAL "")
  set(C_LINK_ARGS_VAL "${RB_TOOLCHAIN_LINK_FLAGS}")
  set(CPP_LINK_ARGS_VAL "${RB_TOOLCHAIN_LINK_FLAGS}")
endif()

# * 编译参数在原有内容后追加额外标志，链接参数却被整串替换。
# * 因此 target、sysroot 或运行库链接参数可能丢失，注释不能把它描述为已完整合并。
if(NOT "${RB_EXTRA_FLAGS}" STREQUAL "")
  set(C_ARGS_VAL "${C_ARGS_VAL} ${RB_EXTRA_FLAGS}")
  set(CPP_ARGS_VAL "${CPP_ARGS_VAL} ${RB_EXTRA_FLAGS}")
  set(C_LINK_ARGS_VAL "${RB_EXTRA_FLAGS}")
  set(CPP_LINK_ARGS_VAL "${RB_EXTRA_FLAGS}")
  # 日志只输出 LTO 串，不展示被覆盖的旧链接参数，诊断时应同时检查工具链输入。
  message(STATUS "Rubberband Extra Build Flags (LTO): ${RB_EXTRA_FLAGS}")
endif()

# * 定义项目路径配置
# * 源码位置按调用目录解释后转绝对路径，不根据脚本目录自动修正入口变化。
get_filename_component(
  RUBBERBAND_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../sources/rubberband"
  ABSOLUTE)
# * 构建与私有安装目录均位于顶层二进制树，同一目录不能混用多套工具链产物。
set(RUBBERBAND_BUILD_DIR "${CMAKE_BINARY_DIR}/rb_bld")
set(RUBBERBAND_INSTALL_DIR "${CMAKE_BINARY_DIR}/rb_inst")
# 当前路径没有平台或配置层，不能直接当作跨工具链共享的预编译根。 * 时间戳用于非 MSVC 的编译/安装短路，MSVC 分支不读取或更新此戳。
set(RUBBERBAND_SOURCE_STAMP
    "${RUBBERBAND_INSTALL_DIR}/.ice_rubberband_sources.stamp")
# * 只比较源码文件 mtime，不识别源码删除、保留时间的替换或安装库被移除。
# * 参数、依赖库内容及生成的 pkg-config 文件变化不在戳失效判断中。
# * POSIX find/grep 与单引号路径属于该检查的环境要求，不支持任意特殊字符路径。
set(RUBBERBAND_SOURCE_READY_TEST
    "test -f '${RUBBERBAND_SOURCE_STAMP}' && ! /usr/bin/find '${RUBBERBAND_SOURCE_DIR}' -type f -newer '${RUBBERBAND_SOURCE_STAMP}' ! -path '*/.git/*' -print -quit | /usr/bin/grep -q ."
)

# * Rubber Band 跟随 ICE_LINKAGE 生成静态库或 DLL。
# * 静态/动态类型来自 ICE_LINKAGE，与后面的 CRT 选择分别处理，不能混为同一开关。
set(RUBBERBAND_LIBRARY_KIND static)
if(ICE_LINKAGE STREQUAL "shared")
  set(RUBBERBAND_LIBRARY_KIND shared)
endif()
set(RUBBERBAND_RUNTIME_LIBRARY "")
# 运行时输出仅在后面的 MSVC shared 分支赋值，不能据空值推断其他平台没有运行时文件。

# * 本地依赖路径 (FFTW3 和 libsamplerate) 快速傅里叶变换库来自 ExternalProject_Add (fftw_project)
# * FFTW 头与库来自前置外部项目安装，目录存在不等于目标文件已完成安装。
set(FFTW_INST_DIR "${CMAKE_BINARY_DIR}/3rdpty/fftw_inst")
set(LOCAL_FFTW3_INCLUDE "${FFTW_INST_DIR}/include")
# FFTW 提供频域计算；该目录只描述其公共头，不代替库目标中的链接要求。
set(LOCAL_FFTW3_LIB_DIR "${FFTW_INST_DIR}/lib")
# 这里固定 lib，与 Buildfftw 安装约定绑定，不通过系统搜索路径猜测 lib64。

# 采样率库来自 Buildlibsamplerate.cmake (samplerate 目标)
set(SAMPLERATE_SRC_DIR "${CMAKE_CURRENT_LIST_DIR}/../libsamplerate")
set(SAMPLERATE_BIN_DIR "${CMAKE_BINARY_DIR}/3rdpty/libsamplerate")
# 采样率库由普通 CMake 目标产生，与 FFTW 的外部安装生命周期不同。 * 此历史组合变量当前未用于最终参数，下面重新按源码头和生成目录建立列表。
set(LOCAL_SAMPLERATE_INCLUDE "${SAMPLERATE_SRC_DIR}/src;${SAMPLERATE_BIN_DIR}")
set(LOCAL_SAMPLERATE_LIB_DIR "${CMAKE_BINARY_DIR}/3rdpty/libsamplerate")
# 此目录变量不从 samplerate 目标查询，目标输出布局改变时必须同步更新这里。

# * 采样率库同时需要源码公共头与构建生成目录，不能只保留其中一个路径。
set(EXTRA_INC_LIST "${LOCAL_FFTW3_INCLUDE}" "${SAMPLERATE_SRC_DIR}/src"
                   "${SAMPLERATE_BIN_DIR}")
set(EXTRA_LIB_LIST "${LOCAL_FFTW3_LIB_DIR}" "${LOCAL_SAMPLERATE_LIB_DIR}")
# 手动拼接目录不按生成器配置附加子目录，多配置产物位置仍需单独验证。

# * 将列表转换为逗号分隔字符串，供 Meson 数组参数使用
# * 逗号用于 Meson 的目录列表参数，含逗号的真实路径没有额外转义支持。
string(REPLACE ";" "," EXTRA_INC_STR "${EXTRA_INC_LIST}")
string(REPLACE ";" "," EXTRA_LIB_STR "${EXTRA_LIB_LIST}")
# 目录列表转换只改变分隔符，不校验目录中的库架构或链接类型。

# * 为本地构建的依赖提供 pkg-config 文件，避免 Meson 意外发现 避免使用 Homebrew 或系统中的不兼容链接版本。
# * 生成的 .pc 属于构建派生文件，不放入第三方源码或发布预编译包目录。
# * 它们为 Meson 提供本地目标元数据，不验证所描述的库已经存在或 ABI 一致。
set(RUBBERBAND_PKG_CONFIG_DIR "${CMAKE_BINARY_DIR}/3rdpty/rubberband_pkgconfig")
file(MAKE_DIRECTORY "${RUBBERBAND_PKG_CONFIG_DIR}")
# 每次配置整体重写元数据，只作用于构建树中的固定派生文件。 * FFTW 版本字段固定为 3.3.10，不从实际源码、头或归档中读取版本。 * Libs
# 使用常规 -lfftw3 名称，目标格式和搜索后缀还需由 pkg-config/工具链正确适配。
file(
  WRITE "${RUBBERBAND_PKG_CONFIG_DIR}/fftw3.pc"
  "prefix=${FFTW_INST_DIR}\n"
  "exec_prefix=${FFTW_INST_DIR}\n"
  # 路径直接展开成当前构建绝对位置，生成的 .pc 不适合作为可迁移发布包元数据。
  "libdir=${LOCAL_FFTW3_LIB_DIR}\n"
  "includedir=${LOCAL_FFTW3_INCLUDE}\n"
  "\n"
  "Name: FFTW\n"
  # Name/Description 供探测诊断显示，不参与 ABI 或对象格式验证。
  "Description: fast Fourier transform library\n"
  "Version: 3.3.10\n"
  # 版本只是探测输入，不校验已经安装的 FFTW 与此声明是否一致。
  "Libs: -L${LOCAL_FFTW3_LIB_DIR} -lfftw3\n"
  # * 静态探测会消费私有数学库依赖，Windows/MSVC 是否正确转换仍需实际配置验证。
  "Libs.private: -lm\n"
  "Cflags: -I${LOCAL_FFTW3_INCLUDE}\n")
# * 采样率库元数据固定 0.1.9，不能把版本字符串当作当前源码版本的证据。
# * 生成配置头目录也传入 Cflags，保证探针与真实库编译看到一致的能力宏输入。
file(
  WRITE "${RUBBERBAND_PKG_CONFIG_DIR}/samplerate.pc"
  "prefix=${SAMPLERATE_BIN_DIR}\n"
  "exec_prefix=${SAMPLERATE_BIN_DIR}\n"
  "libdir=${LOCAL_SAMPLERATE_LIB_DIR}\n"
  "includedir=${SAMPLERATE_SRC_DIR}/src\n"
  "configincludedir=${SAMPLERATE_BIN_DIR}\n"
  # configincludedir 描述构建派生头位置，与源码公共头路径保持分离。
  "\n"
  "Name: libsamplerate\n"
  # 该描述文件名匹配 Meson 的依赖查询入口，不是实际 CMake target 名。
  "Description: Sample Rate Converter for audio\n"
  "Version: 0.1.9\n"
  # 此处没有自动提取 samplerate target 的传递依赖，元数据需与真实目标手工保持一致。
  "Libs: -L${LOCAL_SAMPLERATE_LIB_DIR} -lsamplerate\n"
  "Cflags: -I${SAMPLERATE_SRC_DIR}/src -I${SAMPLERATE_BIN_DIR}\n")

if(MSVC)
  # * MSVC 的 CRT 偏好兼看父级运行库属性与链接偏好，不只看依赖自身库类型。
  set(RUBBERBAND_USE_DLL_CRT OFF)
  # * 任一共享偏好都选择 DLL CRT，不能用显式静态 CRT 字符串强行覆盖这个 OR 条件。
  if(CMAKE_MSVC_RUNTIME_LIBRARY MATCHES "DLL"
     OR ICE_LINKAGE STREQUAL "shared"
     OR PROJECT_LINKAGE STREQUAL "shared")
    set(RUBBERBAND_USE_DLL_CRT ON)
    # 使用 DLL CRT 时 Debug 仍需选 mdd，不可把发布 md 用于所有配置。
  endif()

  # * 下面的 RUBBERBAND_BUILD_TYPE 是历史局部变量，最终 Meson 使用的是开头的 RB_BUILD_TYPE。
  # * CRT 的 debug/release 选择仍按精确 Debug 判断，RelWithDebInfo 使用发布 CRT。 根据主项目的构建类型决定
  #   Meson 的参数
  if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(RUBBERBAND_BUILD_TYPE "debug")
    if(RUBBERBAND_USE_DLL_CRT)
      set(RUBBERBAND_CRT "mdd")
    else()
      set(RUBBERBAND_CRT "mtd")
    endif()
  else()
    set(RUBBERBAND_BUILD_TYPE "release")
    # 这个局部类型值不替代前面的 debugoptimized，实际 setup 参数仍使用 RB_BUILD_TYPE。
    if(RUBBERBAND_USE_DLL_CRT)
      set(RUBBERBAND_CRT "md")
    else()
      set(RUBBERBAND_CRT "mt")
    endif()
  endif()

  # 找到 pkg-config * 先按固定工具目录搜索 pkg-config/pkgconf，再回退 PATH，选择顺序并不绑定当前编译器。
  find_program(
    RUBBERBAND_PKG_CONFIG_EXE
    NAMES pkg-config pkgconf
    PATHS "C:/msys64/ucrt64/bin" "C:/msys64/clang64/bin" "C:/msys64/mingw64/bin"
    NO_DEFAULT_PATH)
  if(NOT RUBBERBAND_PKG_CONFIG_EXE)
    find_program(RUBBERBAND_PKG_CONFIG_EXE NAMES pkg-config pkgconf)
  endif()
  if(NOT RUBBERBAND_PKG_CONFIG_EXE)
    # * 找不到时保留裸命令名而非立即失败，后续 Meson 执行阶段仍可能因工具缺失报错。
    set(RUBBERBAND_PKG_CONFIG_EXE "pkg-config")
  endif()

  # 构造环境变量 * 此环境列表当前没有用于 ExternalProject 命令，实际命令下方重新展开环境设置。 * 因此不能以这里清空
  # PKG_CONFIG_PATH 推断真实配置阶段不使用本地 .pc 目录。
  set(MESON_ENV
      ${CMAKE_COMMAND} -E env "PKG_CONFIG=${RUBBERBAND_PKG_CONFIG_EXE}"
      "PKG_CONFIG_PATH=" # 清空它，确保不使用系统包
      "CMAKE_PREFIX_PATH=")

  # 构造参数 * 配置参数保持列表形式，路径与参数边界在直接执行分支中由命令调用机制传递。 * 测试、CLI 与插件接口都被裁剪，仅构建引擎依赖的库接口。
  set(MESON_SETUP_ARGS
      --prefix=${RUBBERBAND_INSTALL_DIR}
      --libdir=lib
      --buildtype=${RB_BUILD_TYPE}
      # * Meson 的 CRT 取值独立于 default_library，动态 CRT 并不意味着必然生成共享库。
      -Db_vscrt=${RUBBERBAND_CRT}
      -Ddefault_library=${RUBBERBAND_LIBRARY_KIND}
      # 默认库类型在 setup 时固化，非 MSVC 已有 build.ninja 的短路逻辑不会自动同步切换。 * 关闭上游测试不代替 ICE
      # 数值和实时验证，构建成功不能证明算法结果正确。 当前 MSVC 标准库至少要求 C++14，显式覆盖上游的 C++11 默认值。
      -Dcpp_std=c++17
      -Dtests=disabled
      -Dcmdline=disabled
      # * Vamp、LADSPA、LV2 是独立插件入口，这里不把它们作为引擎嵌入 API 交付。
      -Dvamp=disabled
      -Dladspa=disabled
      -Dlv2=disabled
      # 插件协议禁用不影响核心 C++ 库的消费目标，不能以此判断引擎脚本绑定能力。 * JNI 不属于当前 C++ 消费接口，禁用后不会生成 Java
      # 绑定产物。
      -Djni=disabled
      # * 固定 FFTW 和 libsamplerate 实现选择，避免 Meson 自动选另一套系统后端。
      -Dfft=fftw
      -Dresampler=libsamplerate
      # include/lib 参数只是提供搜索目录，不代替前置依赖必须已构建的目标关系。
      "-Dextra_include_dirs=${EXTRA_INC_STR}"
      "-Dextra_lib_dirs=${EXTRA_LIB_STR}")

  # * 只有非空参数才追加，避免传入空覆盖值改变 Meson 默认参数处理。
  if(NOT "${C_ARGS_VAL}" STREQUAL "")
    list(APPEND MESON_SETUP_ARGS "-Dc_args=${C_ARGS_VAL}")
  endif()
  if(NOT "${CPP_ARGS_VAL}" STREQUAL "")
    list(APPEND MESON_SETUP_ARGS "-Dcpp_args=${CPP_ARGS_VAL}")
  endif()

  # * 两种语言的链接选项一起追加；进入条件只检查 C 链接串，二者应同步维护。
  if(NOT "${C_LINK_ARGS_VAL}" STREQUAL "")
    list(APPEND MESON_SETUP_ARGS "-Dc_link_args=${C_LINK_ARGS_VAL}"
         "-Dcpp_link_args=${CPP_LINK_ARGS_VAL}")
  endif()

  # 由当前 CMake 工具链生成机器文件，避免 PATH 中未带版本的 LLVM 与主构建不同。
  # 文件位于构建目录，使独立引擎构建不依赖上层项目的交叉配置文件。
  if(CMAKE_CROSSCOMPILING)
    set(_rb_cross_file "${CMAKE_BINARY_DIR}/3rdpty/rubberband-msvc-cross.ini")
    # Meson 按可执行文件名识别 clang-cl；版本后缀会被误识别为 GNU 驱动。 构建目录内的标准名称链接仍指向 CMake
    # 已选定的同版本编译器。
    set(_rb_compiler "${CMAKE_BINARY_DIR}/3rdpty/rubberband-tools/clang-cl")
    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/3rdpty/rubberband-tools")
    file(CREATE_LINK "${CMAKE_C_COMPILER}" "${_rb_compiler}" SYMBOLIC)
    # 归档器也按标准名称识别，避免 llvm-lib-22 被当成 GNU ar 使用 csr 参数。
    set(_rb_archiver "${CMAKE_BINARY_DIR}/3rdpty/rubberband-tools/llvm-lib")
    file(CREATE_LINK "${CMAKE_AR}" "${_rb_archiver}" SYMBOLIC)
    # * 目标固定为当前交叉入口的 x86_64 Windows，不能使用宿主 Linux 的 ABI 探测值。
    # * needs_exe_wrapper 禁止配置阶段直接在宿主运行目标程序。
    # * c_ld 服务直接链接，编译驱动探针则由 -fuse-ld=lld 选择同目录链接器。
    file(
      WRITE "${_rb_cross_file}"
      "[binaries]\nc = ['${_rb_compiler}', '-fuse-ld=lld']\ncpp = ['${_rb_compiler}', '-fuse-ld=lld']\nar = '${_rb_archiver}'\nc_ld = '${CMAKE_LINKER}'\ncpp_ld = '${CMAKE_LINKER}'\n[host_machine]\nsystem = 'windows'\ncpu_family = 'x86_64'\ncpu = 'x86_64'\nendian = 'little'\n[properties]\nneeds_exe_wrapper = true\n"
    )
    list(APPEND MESON_SETUP_ARGS "--cross-file=${_rb_cross_file}")
  endif()
  # Meson 在重配置时缓存编译器和归档器类型；机器文件变化必须清除旧探测结果。 只在已有配置的外部 configure
  # 步骤重建私有构建树，普通增量编译不执行此步骤。
  set(_rb_setup_mode --reconfigure)
  if(EXISTS "${RUBBERBAND_BUILD_DIR}/meson-private/coredata.dat")
    set(_rb_setup_mode --wipe)
  endif()
  # 空 native 文件使用实际路径，Linux 不把 NUL 识别为 Windows 空设备。
  set(_rb_native_file "${CMAKE_BINARY_DIR}/3rdpty/rubberband-native.ini")
  file(WRITE "${_rb_native_file}" "# 不覆盖宿主编译器选项。\n")

  # 确定库文件产物路径 * MSVC 共享模式分别登记导入库和 DLL，静态模式消费带 static 后缀的归档。
  if(ICE_LINKAGE STREQUAL "shared")
    set(RUBBERBAND_LIBRARY "${RUBBERBAND_INSTALL_DIR}/lib/rubberband.lib")
    # 导入库负责链接，实际运行还需 bin 下 DLL；不能只登记其中一个就当包完整。
    set(RUBBERBAND_RUNTIME_LIBRARY
        "${RUBBERBAND_INSTALL_DIR}/bin/rubberband.dll")
  else()
    set(RUBBERBAND_LIBRARY
        "${RUBBERBAND_INSTALL_DIR}/lib/rubberband-static.lib")
  endif()

  # * 上游在 clang-cl 下仍安装 librubberband.a，但归档内容已经是 MSVC COFF。
  # * 安装后统一名称，保持消费接口及预编译打包脚本的 rubberband-static.lib 契约。
  # * 这里只复制字节，不剥离 CodeView，也不重新归档；原始安装文件继续保留。
  set(_rb_normalize_command "")
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND ICE_LINKAGE STREQUAL "static")
    set(_rb_normalize_command
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${RUBBERBAND_INSTALL_DIR}/lib/librubberband.a" "${RUBBERBAND_LIBRARY}")
  endif()

  # * 这个分支不使用上述 POSIX 时间戳检查，不能将非 MSVC 增量缓存语义套用到这里。 使用 CMake 环境包装器，避免 MSVC 构建依赖类
  #   Unix 命令行外壳。
  # * 只使用工作区现有源码，外部更新步骤为空，不触发仓库下载或源码替换。
  ExternalProject_Add(
    rubberband_project
    LIST_SEPARATOR "__ICE_RB_SEPARATOR__"
    SOURCE_DIR "${RUBBERBAND_SOURCE_DIR}"
    BINARY_DIR "${RUBBERBAND_BUILD_DIR}"
    # * 依赖边同时包含 ExternalProject 与普通 CMake 库目标，确保 Meson 探针前库已就绪。
    DEPENDS fftw_project samplerate
    UPDATE_COMMAND ""
    # * 每次进入 Meson compile，由 Meson 自身处理增量，不等于每轮重编所有对象。
    BUILD_ALWAYS TRUE
    CONFIGURE_COMMAND
      ${CMAKE_COMMAND} -E env ${RB_MSVC_ENV}
      "PKG_CONFIG=${RUBBERBAND_PKG_CONFIG_EXE}"
      "PKG_CONFIG_PATH=${RUBBERBAND_PKG_CONFIG_DIR}"
      "PKG_CONFIG_LIBDIR=${RUBBERBAND_PKG_CONFIG_DIR}" "CMAKE_PREFIX_PATH="
      # * 真实配置环境将 PATH/LIBDIR 都指向本地元数据，并清空 CMAKE_PREFIX_PATH。 使用生成的空 native
      #   文件，不依赖当前 shell 的空设备名称。
      "${RUBBERBAND_MESON_EXE}" setup ${_rb_setup_mode} ${MESON_SETUP_ARGS}
      --native-file=${_rb_native_file} "${RUBBERBAND_BUILD_DIR}"
      "${RUBBERBAND_SOURCE_DIR}"
    # * 编译阶段保持相同依赖搜索环境，避免 Meson 重新检测时回到系统包。
    BUILD_COMMAND
      ${CMAKE_COMMAND} -E env ${RB_MSVC_ENV}
      "PKG_CONFIG=${RUBBERBAND_PKG_CONFIG_EXE}"
      "PKG_CONFIG_PATH=${RUBBERBAND_PKG_CONFIG_DIR}"
      "PKG_CONFIG_LIBDIR=${RUBBERBAND_PKG_CONFIG_DIR}" "CMAKE_PREFIX_PATH="
      "${RUBBERBAND_MESON_EXE}" compile -C "${RUBBERBAND_BUILD_DIR}"
    INSTALL_COMMAND
      ${CMAKE_COMMAND} -E env ${RB_MSVC_ENV}
      "PKG_CONFIG=${RUBBERBAND_PKG_CONFIG_EXE}"
      "PKG_CONFIG_PATH=${RUBBERBAND_PKG_CONFIG_DIR}"
      "PKG_CONFIG_LIBDIR=${RUBBERBAND_PKG_CONFIG_DIR}" "CMAKE_PREFIX_PATH="
      "${RUBBERBAND_MESON_EXE}" install -C "${RUBBERBAND_BUILD_DIR}"
      # * 安装不再次触发构建，要求前一编译步骤已成功，失败由外部项目命令传播。
      --no-rebuild ${_rb_normalize_command}
    # 当前 byproducts 不包含 PDB，构建成功不能作为旁路符号打包完整的证明。
    BUILD_BYPRODUCTS "${RUBBERBAND_LIBRARY}" ${RUBBERBAND_RUNTIME_LIBRARY})
  # * 非 MSVC 分支使用 shell 环境字符串和源码戳，与上方直接命令执行分支语义不同。
else()
  # 构造 Meson 配置参数 非 MSVC 参数列表独立维护，调整功能开关时需同步核对上方 MSVC 分支。
  set(MESON_SETUP_ARGS
      --prefix=${RUBBERBAND_INSTALL_DIR}
      --libdir=lib
      --buildtype=${RB_BUILD_TYPE}
      -Ddefault_library=${RUBBERBAND_LIBRARY_KIND}
      -Dtests=disabled
      -Dcmdline=disabled
      -Dvamp=disabled
      -Dladspa=disabled
      -Dlv2=disabled
      -Djni=disabled
      -Dfft=fftw
      -Dresampler=libsamplerate
      "-Dextra_include_dirs=${EXTRA_INC_STR}"
      "-Dextra_lib_dirs=${EXTRA_LIB_STR}")

  if(NOT "${C_ARGS_VAL}" STREQUAL "")
    list(APPEND MESON_SETUP_ARGS "-Dc_args=${C_ARGS_VAL}")
  endif()
  if(NOT "${CPP_ARGS_VAL}" STREQUAL "")
    list(APPEND MESON_SETUP_ARGS "-Dcpp_args=${CPP_ARGS_VAL}")
  endif()

  if(NOT "${C_LINK_ARGS_VAL}" STREQUAL "")
    list(APPEND MESON_SETUP_ARGS "-Dc_link_args=${C_LINK_ARGS_VAL}"
         "-Dcpp_link_args=${CPP_LINK_ARGS_VAL}")
  endif()

  if(CMAKE_CROSSCOMPILING)
    # Meson 只有拿到 cross file 后才会跳过运行目标平台的 sanity exe；MinGW 交叉构建必须显式声明 Windows
    # host machine 和配套 binutils。 * 此分支针对所有非 MSVC 交叉构建，但生成内容固定 Windows x86_64
    # little endian。 * 其他交叉目标不能仅靠修改 CMake 编译器就视为已支持，需要另行扩展机器描述。
    set(RUBBERBAND_MESON_CROSS_FILE
        "${CMAKE_BINARY_DIR}/3rdpty/rubberband-mingw-cross.ini")
    file(
      WRITE "${RUBBERBAND_MESON_CROSS_FILE}"
      # 这里生成配置文本，不执行目标程序，也没有检测目标位宽是否与硬编码机器描述一致。 * 工具来自目标 CMake 工具链，pkg-config
      # 则保留裸命令名由宿主环境解析。
      "[binaries]\n"
      "c = '${CMAKE_C_COMPILER}'\n"
      "cpp = '${CMAKE_CXX_COMPILER}'\n"
      # 归档与符号工具从父级取值；提供 strip 工具路径本身不会剥离库符号。
      "ar = '${CMAKE_AR}'\n"
      # ranlib 与 ar 分别声明，不能仅通过库文件扩展名推断索引工具。
      "ranlib = '${CMAKE_RANLIB}'\n"
      "nm = '${CMAKE_NM}'\n"
      # 这些路径按字符串写入 INI，内嵌单引号没有专门转义，工具链路径需满足引用约束。
      "strip = '${CMAKE_STRIP}'\n"
      # * 资源编译工具作为目标 binutils 配套项，传入路径不验证该工具实际可用。
      "windres = '${CMAKE_RC_COMPILER}'\n"
      "pkg-config = 'pkg-config'\n"
      # 裸 pkg-config 由外部步骤环境解析，本分支未复用 MSVC 的工具查找结果。
      "\n"
      # * Meson 的 host_machine 表示生成代码的目标，不是运行 Meson 的宿主机器。
      "[host_machine]\n"
      "system = 'windows'\n"
      "cpu_family = 'x86_64'\n"
      # cpu 与 family 同时固定，不能因 CMAKE_SYSTEM_PROCESSOR 改为 ARM 就自动适配。
      "cpu = 'x86_64'\n"
      "endian = 'little'\n"
      "\n"
      "[properties]\n"
      # * 明确目标程序不能直接在构建宿主运行，不在此提供模拟器或执行包装器。
      "needs_exe_wrapper = true\n")
    # * 生成文件放在构建树，重复配置会覆盖派生内容，不修改版本控制中的工具链文件。
    list(APPEND MESON_SETUP_ARGS "--cross-file=${RUBBERBAND_MESON_CROSS_FILE}")
  endif()

  if(ICE_LINKAGE STREQUAL "shared")
    set(RUBBERBAND_LIBRARY
        # * 非 MSVC 共享路径直接拼共享库后缀，没有单独登记 MinGW 导入库及 bin 下 DLL。
        "${RUBBERBAND_INSTALL_DIR}/lib/${CMAKE_SHARED_LIBRARY_PREFIX}rubberband${CMAKE_SHARED_LIBRARY_SUFFIX}"
    )
  else()
    set(RUBBERBAND_LIBRARY "${RUBBERBAND_INSTALL_DIR}/lib/librubberband.a")
    # 静态归档无配置后缀，不同配置需要独立构建与安装前缀隔离。
  endif()

  # 将列表转换为字符串，以便在 sh -c 中使用
  set(MESON_SETUP_ARGS_STR "")
  # 每次重新建立命令串，不把旧列表项残留到新的 setup 请求中。 * 逐项套双引号不完整转义内嵌引号、美元符号等 shell
  # 字符，不能保证任意路径安全。
  foreach(arg IN LISTS MESON_SETUP_ARGS)
    set(MESON_SETUP_ARGS_STR "${MESON_SETUP_ARGS_STR} \"${arg}\"")
  endforeach()

  # 构建时 Meson 不会自动继承 CMake 的编译器和工具链选择。 * 编译器和 binutils 在环境中显式传递，Meson 不会自动继承父
  # CMake 缓存。 * 环境值用单引号嵌入 shell，路径含单引号时仍需额外处理。
  set(MESON_TOOLCHAIN_ENV
      "CC='${CMAKE_C_COMPILER}' CXX='${CMAKE_CXX_COMPILER}' AR='${CMAKE_AR}' RANLIB='${CMAKE_RANLIB}' NM='${CMAKE_NM}' PKG_CONFIG_PATH='${RUBBERBAND_PKG_CONFIG_DIR}' PKG_CONFIG_LIBDIR='${RUBBERBAND_PKG_CONFIG_DIR}'"
  )

  ExternalProject_Add(
    rubberband_project
    SOURCE_DIR "${RUBBERBAND_SOURCE_DIR}"
    BINARY_DIR "${RUBBERBAND_BUILD_DIR}"
    DEPENDS fftw_project samplerate
    UPDATE_COMMAND ""
    BUILD_ALWAYS TRUE
    CONFIGURE_COMMAND
      sh -c
      # * 只要 build.ninja 存在就跳过 setup，修改配置参数不保证已有 Meson 构建树重新配置。
      "test -f '${RUBBERBAND_BUILD_DIR}/build.ninja' || ${MESON_TOOLCHAIN_ENV} '${RUBBERBAND_MESON_EXE}' setup ${MESON_SETUP_ARGS_STR} '${RUBBERBAND_BUILD_DIR}' '${RUBBERBAND_SOURCE_DIR}'"
    # 存在 build.ninja 仅证明曾生成过构建树，不能证明当前选项与该构建树一致。
    BUILD_COMMAND
      sh -c
      # * 戳有效时整个 compile 被短路，Meson 自身也没有机会检查依赖或参数变化。
      "${RUBBERBAND_SOURCE_READY_TEST} || ${MESON_TOOLCHAIN_ENV} '${RUBBERBAND_MESON_EXE}' compile -C '${RUBBERBAND_BUILD_DIR}'"
    # 此命令未显式传入并行上限，不能声称 Meson 子构建严格沿用外层 Ninja -j。
    INSTALL_COMMAND
      sh -c
      # * 安装成功才更新戳；--no-rebuild 假定前一步编译确已产生需要安装的产物。
      "${RUBBERBAND_SOURCE_READY_TEST} || (${MESON_TOOLCHAIN_ENV} '${RUBBERBAND_MESON_EXE}' install -C '${RUBBERBAND_BUILD_DIR}' --no-rebuild && '${CMAKE_COMMAND}' -E touch '${RUBBERBAND_SOURCE_STAMP}')"
    # 失败时保留诊断产物但不写成功戳，脚本没有自动清理或回滚已部分安装的内容。 * 这里登记链接库和戳，不登记非 MSVC 的运行时
    # DLL或符号文件，不能代表完整发布包。
    BUILD_BYPRODUCTS "${RUBBERBAND_LIBRARY}" "${RUBBERBAND_SOURCE_STAMP}")
endif()

# * 封装为 CMake 接口库
# * 配置期先建公共头目录，仅满足路径检查，不表示外部库已经完成安装。
file(MAKE_DIRECTORY "${RUBBERBAND_INSTALL_DIR}/include")

# * 稳定包装名与预编译模式一致，不把 Meson 构建细节暴露给业务消费者。
add_library(3rd_rubberband INTERFACE)
add_dependencies(3rd_rubberband rubberband_project)
# 此依赖保证包装目标使用前先完成安装，不表示 Meson 本身是常驻运行时组件。

target_include_directories(3rd_rubberband
                           INTERFACE "${RUBBERBAND_INSTALL_DIR}/include")
# 只公开安装后的接口头，业务不依赖 Meson 中间构建目录的私有头布局。 * 消费安装后的真实库路径，构建依赖负责排序，不重新合并其内部对象。
target_link_libraries(3rd_rubberband INTERFACE "${RUBBERBAND_LIBRARY}")

# * 链接依赖的静态库
# * 传播两套依赖目标的链接要求，不在这里假定它们必然都是静态库。
target_link_libraries(3rd_rubberband INTERFACE 3rd_fftw3 3rd_libsamplerate)

# * 系统底层库链接
# * 系统库按目标平台分支，MinGW 补 pthread，MSVC 不使用这一 Unix 线程链接名。
if(WIN32)
  if(NOT MSVC)
    target_link_libraries(3rd_rubberband INTERFACE pthread)
  endif()
else()
  # * 非 Windows 补数学库；Apple 分支不再显式加 pthread，其他 Unix 保留线程依赖。
  target_link_libraries(3rd_rubberband INTERFACE m)
  # 系统库只作为链接要求传播，不由本项目下载安装或复制到预编译包中。
  if(NOT APPLE)
    target_link_libraries(3rd_rubberband INTERFACE pthread)
  endif()
endif()
