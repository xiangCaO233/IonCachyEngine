# 配置 LAME 的 Autotools 外部构建，为 FFmpeg 提供稳定的 MP3 编码静态库接口。

include(ExternalProject)
include(ProcessorCount)
include("${PROJECT_SOURCE_DIR}/cmake/ICEMsvcExternalEnvironment.cmake")
# 配置、编译及安装复用同一 SDK 环境，增量构建不依赖原终端。
ice_msvc_external_environment(ICE_LAME_ENV "__ICE_LAME_SEPARATOR__")

ProcessorCount(ICE_LAME_PROCESSOR_COUNT)
# 子 make 使用独立探测的 CPU 数，不继承外层 Ninja 的并行任务上限。
if(ICE_LAME_PROCESSOR_COUNT EQUAL 0)
  # 探测失败时串行构建，避免把 -j0 交给 make 产生不期望的并行策略。
  set(ICE_LAME_PROCESSOR_COUNT 1)
endif()

# 源码相对于脚本定位，构建和安装目录集中在顶层二进制树，避免污染上游源码。
set(ICE_LAME_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/../lame")
set(ICE_LAME_BINARY_DIR "${CMAKE_BINARY_DIR}/3rdpty/lame_bld")
# 此安装前缀只服务当前构建，不是按架构、工具链和配置隔离的发布预编译布局。
set(ICE_LAME_INSTALL_DIR "${CMAKE_BINARY_DIR}/3rdpty/lame_inst")
set(ICE_LAME_INCLUDE_DIR "${ICE_LAME_INSTALL_DIR}/include")
# 固定 lib 而非依赖发行版的 lib64 默认值，使探测和消费路径保持一致。
set(ICE_LAME_LIBRARY_DIR "${ICE_LAME_INSTALL_DIR}/lib")
# 戳只有安装和必要的归档合并全部成功才更新，失败后下一次仍应尝试完成安装。
set(ICE_LAME_SOURCE_STAMP "${ICE_LAME_INSTALL_DIR}/.ice_lame_sources.stamp")
# * 缓存只比较源码文件的 mtime，不识别源码删除、保留时间的替换或安装库丢失。
# * 编译选项变化不在此戳的输入中，换工具链或配置应使用隔离构建树。
# * 表达式依赖 POSIX 工具；路径嵌入单引号，含单引号的目录名需要额外引用处理。
set(ICE_LAME_SOURCE_READY_TEST
    "test -f '${ICE_LAME_SOURCE_STAMP}' && ! /usr/bin/find '${ICE_LAME_SOURCE_DIR}' -type f -newer '${ICE_LAME_SOURCE_STAMP}' ! -path '*/.git/*' -print -quit | /usr/bin/grep -q ."
)

if(MSVC)
  # 对外消费的最终归档统一叫 mp3lame.lib，不把 Autotools 的中间命名暴露给业务。
  set(ICE_LAME_STATIC_LIBRARY "${ICE_LAME_LIBRARY_DIR}/mp3lame.lib")
  if(CMAKE_CROSSCOMPILING)
    # 交叉 MSVC 先安装 .a 再重建 .lib，二者不是同一个产物路径。
    set(ICE_LAME_BUILD_LIBRARY "${ICE_LAME_LIBRARY_DIR}/libmp3lame.a")
    # 向量对象来自二进制树，合并阶段还需把它补入最终归档。
    set(ICE_LAME_VECTOR_OBJECT
        "${ICE_LAME_BINARY_DIR}/libmp3lame/vector/xmm_quantize_sub.obj")
  else()
    # 原生 MSVC 不经过下面的交叉归档合并，直接要求安装后的 .lib 存在。
    set(ICE_LAME_BUILD_LIBRARY "${ICE_LAME_STATIC_LIBRARY}")
  endif()
else()
  # 其他工具链直接消费 Autotools 安装的归档，没有额外的命名转换步骤。
  set(ICE_LAME_STATIC_LIBRARY "${ICE_LAME_LIBRARY_DIR}/libmp3lame.a")
  set(ICE_LAME_BUILD_LIBRARY "${ICE_LAME_STATIC_LIBRARY}")
endif()

string(TOUPPER "${CMAKE_BUILD_TYPE}" ICE_LAME_BUILD_TYPE_UPPER)
# 使用配置名索引父级 C flags；此入口没有为多配置生成器建立独立配置产物映射。
set(ICE_LAME_CONFIG_C_FLAGS "${CMAKE_C_FLAGS_${ICE_LAME_BUILD_TYPE_UPPER}}")

# autotools 不继承 CMake 配置型 CFLAGS，这里显式拼入 Debug/RelWithDebInfo 的 -g。
# 预编译静态库必须保留符号，便于下游定位第三方依赖问题。
set(ICE_LAME_C_FLAGS "${CMAKE_C_FLAGS} ${ICE_LAME_CONFIG_C_FLAGS}")
# 此处继承的 flags 不应含业务 PGO 参数，第三方依赖不可随主项目一起插桩。 仅取通用 EXE 链接参数，配置专属的链接 flags 并未在此拼入。
set(ICE_LAME_LINK_FLAGS "${CMAKE_EXE_LINKER_FLAGS}")
if(CMAKE_C_COMPILER_TARGET)
  # 编译与 configure 链接探针都需要 target triple，不能只改变对象生成目标。
  string(APPEND ICE_LAME_C_FLAGS " --target=${CMAKE_C_COMPILER_TARGET}")
  string(APPEND ICE_LAME_LINK_FLAGS " --target=${CMAKE_C_COMPILER_TARGET}")
endif()
if(CMAKE_SYSROOT)
  # 探针头文件和链接搜索都应使用目标 sysroot，避免混入宿主 ABI。
  string(APPEND ICE_LAME_C_FLAGS " --sysroot=${CMAKE_SYSROOT}")
  string(APPEND ICE_LAME_LINK_FLAGS " --sysroot=${CMAKE_SYSROOT}")
endif()
if(NOT MSVC)
  # 非 MSVC 静态归档保留 PIC，以便最终被嵌入共享库，不改变 LAME 的静态库类型。
  string(APPEND ICE_LAME_C_FLAGS " -fPIC")
endif()
string(STRIP "${ICE_LAME_C_FLAGS}" ICE_LAME_C_FLAGS)
# 仅去掉首尾空白，不重写用户 flags 内部的引号或参数分组。
string(STRIP "${ICE_LAME_LINK_FLAGS}" ICE_LAME_LINK_FLAGS)

set(ICE_LAME_CONFIGURE_SCRIPT "${ICE_LAME_SOURCE_DIR}/configure")
# 默认直接使用父工具链驱动，只有交叉 MSVC 分支换成参数适配包装器。
set(ICE_LAME_CC "${CMAKE_C_COMPILER}")
set(ICE_LAME_AR "${CMAKE_AR}")
if(MSVC AND CMAKE_CROSSCOMPILING)
  # LAME 的 Autotools 接口使用 GCC 风格探测参数；包装器只做参数协议适配， 实际目标对象和静态库仍分别由 clang-cl 与
  # llvm-lib 生成。
  set(ICE_LAME_CC
      "${PROJECT_SOURCE_DIR}/cmake/cross/clang-cl-gcc-compatible.sh")
  set(ICE_LAME_AR "${PROJECT_SOURCE_DIR}/cmake/cross/llvm-lib-ar-compatible.sh")
  # 包装器路径依赖 PROJECT_SOURCE_DIR 指向 ICE，嵌入方式改变时需核对该变量归属。
  if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    # 这里整体覆盖前面拼好的 C flags，并固定静态 CRT，不再继承父级 CRT 偏好。
    set(ICE_LAME_C_FLAGS "/MTd /Z7 /Od -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00")
  else()
    # 非精确 Debug 都使用发布 flags；/Z7 保留对象内符号，未按每种发布配置再细分。
    set(ICE_LAME_C_FLAGS "/MT /Z7 /O2 -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00")
  endif()
  # 适配分支清空通用链接参数，避免 GCC 风格 flags 穿透到 MSVC 链接探针。
  set(ICE_LAME_LINK_FLAGS "")
endif()

# 以命令列表传递环境赋值，工具和 flags 只影响本次 configure，不改宿主环境。
set(ICE_LAME_CONFIGURE_COMMAND
    ${CMAKE_COMMAND} -E env ${ICE_LAME_ENV} "CC=${ICE_LAME_CC}"
    "AR=${ICE_LAME_AR}"
    # 工具路径只作为环境输入传递；提供 STRIP 不等于执行安装后符号剥离。
    "RANLIB=${CMAKE_RANLIB}" "NM=${CMAKE_NM}" "STRIP=${CMAKE_STRIP}"
    "CFLAGS=${ICE_LAME_C_FLAGS}" "LDFLAGS=${ICE_LAME_LINK_FLAGS}")

if(WIN32)
  # 配置脚本需 shell 执行；这里只检查可执行程序存在，不验证 make、find 等配套工具。
  find_program(ICE_LAME_SH_EXECUTABLE NAMES sh.exe sh bash.exe bash)
  if(NOT ICE_LAME_SH_EXECUTABLE)
    # 缺 shell 在配置期明确失败，不回退下载工具或改用系统 LAME。
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
  # 三个安装目录显式指定，后续消费不依赖上游或发行版默认前缀。
  --prefix=${ICE_LAME_INSTALL_DIR}
  --libdir=${ICE_LAME_LIBRARY_DIR}
  --includedir=${ICE_LAME_INCLUDE_DIR}
  # 当前入口固定静态 LAME，不根据 ICE_LINKAGE 切换共享库或部署 DLL。
  --disable-shared
  --enable-static
  # configure 的 PIC 请求与非 MSVC C flags 相互配合，不代表生成共享目标。
  --with-pic
  # 只保留编码库，CLI 与 MP3 分析/RTP 工具不进入引擎依赖构建范围。
  --disable-frontend
  --disable-mp3x
  --disable-mp3rtp
  # 不为编码依赖拉入 GTK 探测或 MP3 解码实现，解码由引擎其他路径承担。
  --disable-gtktest
  --disable-decoder
  --disable-libmpg123)

if(MSVC AND CMAKE_CROSSCOMPILING)
  # 使用 MSVC triplet 避免 Autotools 注入 MinGW 兼容代码；目标对象仍由 clang-cl 生成。
  list(APPEND ICE_LAME_CONFIGURE_COMMAND "--host=x86_64-pc-windows")
  # 上述 host 固定 x86_64，不能据此认定此分支支持任意 MSVC 目标架构。
elseif(CMAKE_CROSSCOMPILING AND MINGW_TOOLCHAIN_PREFIX)
  # autotools 无法只靠 CC 判断交叉目标；显式传入 --host，避免 configure 尝试运行 Windows 测试程序。
  list(APPEND ICE_LAME_CONFIGURE_COMMAND "--host=${MINGW_TOOLCHAIN_PREFIX}")
endif()

set(ICE_LAME_INSTALL_ACTION "make install")
# byproducts 告诉 Ninja 外部项目产物的归属，不执行文件存在性或归档内容验证。
set(ICE_LAME_BUILD_BYPRODUCTS "${ICE_LAME_BUILD_LIBRARY}")
if(MSVC AND CMAKE_CROSSCOMPILING)
  # 解包工具是宿主程序，优先匹配编译器目录，不依赖宿主是否提供带版本号的命令别名。 CMAKE_AR 在 MSVC 下是
  # llvm-lib，无法承担展开已有归档的操作。 单独查找 llvm-ar 只用于读归档；最终输出仍由当前 CMAKE_AR 重建。
  get_filename_component(ICE_LAME_COMPILER_DIR "${CMAKE_C_COMPILER}" DIRECTORY)
  find_program(
    ICE_LAME_LLVM_AR
    NAMES llvm-ar-22 llvm-ar
    HINTS "${ICE_LAME_COMPILER_DIR}"
    NO_CMAKE_FIND_ROOT_PATH REQUIRED)
  # LAME 3.100 不会把 x86 SIMD 对象安装进主归档；展开后用 llvm-lib 重建，确保 mp3lame.lib
  # 同时包含主实现和向量实现，且不产生嵌套归档。
  string(
    APPEND
    ICE_LAME_INSTALL_ACTION
    # && 保证安装成功后才合并，合并失败也不会继续写入成功戳。
    " && '${CMAKE_COMMAND}' -E env 'ICE_LLVM_AR=${ICE_LAME_LLVM_AR}' 'ICE_LLVM_LIB=${CMAKE_AR}' '${PROJECT_SOURCE_DIR}/cmake/cross/merge-msvc-archives.sh' '${ICE_LAME_STATIC_LIBRARY}' '${ICE_LAME_BUILD_LIBRARY}' '${ICE_LAME_VECTOR_OBJECT}'"
  )
  # 最终 .lib 和额外向量对象均登记，避免下游只看到原始 .a 的生成关系。
  list(APPEND ICE_LAME_BUILD_BYPRODUCTS "${ICE_LAME_VECTOR_OBJECT}"
       "${ICE_LAME_STATIC_LIBRARY}")
endif()

ExternalProject_Add(
  lame_project
  # 独特占位符只替换环境里的分号，不会改写缓存检查中的 | 或 || 运算符。
  LIST_SEPARATOR "__ICE_LAME_SEPARATOR__"
  # 只消费工作区已有源码，没有仓库 URL，更新步骤显式为空。
  SOURCE_DIR "${ICE_LAME_SOURCE_DIR}"
  BINARY_DIR "${ICE_LAME_BINARY_DIR}"
  INSTALL_DIR "${ICE_LAME_INSTALL_DIR}"
  UPDATE_COMMAND ""
  # 每轮触发缓存检查，但有效戳会短路实际 make 和安装操作。
  BUILD_ALWAYS TRUE
  CONFIGURE_COMMAND ${ICE_LAME_CONFIGURE_COMMAND}
  # 构建阶段仍直接使用 sh，不复用上面仅用于 configure 的 shell 查找结果。
  BUILD_COMMAND
    ${CMAKE_COMMAND} -E env ${ICE_LAME_ENV} sh -c
    "${ICE_LAME_SOURCE_READY_TEST} || make -j${ICE_LAME_PROCESSOR_COUNT}"
  INSTALL_COMMAND
    ${CMAKE_COMMAND} -E env ${ICE_LAME_ENV} sh -c
    "${ICE_LAME_SOURCE_READY_TEST} || (${ICE_LAME_INSTALL_ACTION} && '${CMAKE_COMMAND}' -E touch '${ICE_LAME_SOURCE_STAMP}')"
  # 安装成功戳也属于生成物，清理与增量依赖图需要知道其外部项目归属。
  BUILD_BYPRODUCTS ${ICE_LAME_BUILD_BYPRODUCTS} "${ICE_LAME_SOURCE_STAMP}")

# 配置期预建消费目录，仅保证路径存在，真实头和库仍需等待安装完成。
file(MAKE_DIRECTORY "${ICE_LAME_INCLUDE_DIR}")
file(MAKE_DIRECTORY "${ICE_LAME_LIBRARY_DIR}")

add_library(3rd_lame INTERFACE)
# 稳定接口与预编译模式同名，显式依赖确保最终链接前外部安装和合并已完成。
add_dependencies(3rd_lame lame_project)
target_include_directories(3rd_lame INTERFACE "${ICE_LAME_INCLUDE_DIR}")
# 交叉 MSVC 消费合并后的 .lib，而不是遗漏向量对象的安装中间归档。
target_link_libraries(3rd_lame INTERFACE "${ICE_LAME_STATIC_LIBRARY}")
