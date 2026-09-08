# 配置独立 zlib 子构建，向 FFmpeg 提供静态压缩库及 pkg-config 元数据。

include(ExternalProject)
# 此入口只服务 SOURCES_BUILD=ON；OFF 的缺包错误不得通过包含本文件绕过。

# 以脚本位置定位源码；构建与安装树放在顶层二进制目录，不污染上游源码。
set(ICE_ZLIB_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/../zlib")
set(ICE_ZLIB_BINARY_DIR "${CMAKE_BINARY_DIR}/3rdpty/zlib_bld")
# 同一顶层构建树只有一套 zlib 路径，不能在其中混用不同架构或编译器的产物。 此处是构建私有安装前缀，不是发布预编译包目录，也不会安装到系统前缀。
set(ICE_ZLIB_INSTALL_DIR "${CMAKE_BINARY_DIR}/3rdpty/zlib_inst")
set(ICE_ZLIB_INCLUDE_DIR "${ICE_ZLIB_INSTALL_DIR}/include")
# 固定 lib 而非 lib64，使后续 FFmpeg 探测和安装别名共用同一个目录。
set(ICE_ZLIB_LIBRARY_DIR "${ICE_ZLIB_INSTALL_DIR}/lib")
set(ICE_ZLIB_PKGCONFIG_DIR "${ICE_ZLIB_LIBRARY_DIR}/pkgconfig")
# 时间戳仅在安装和兼容名复制全部成功后更新，失败不能被缓存成完成。
set(ICE_ZLIB_SOURCE_STAMP "${ICE_ZLIB_INSTALL_DIR}/.ice_zlib_sources.stamp")
# 这是文件时间启发式，不是内容哈希；删除文件或保留 mtime 的替换不会被识别。 固定使用 POSIX 工具，Windows 构建也必须在提供
# sh、find、grep 的环境运行。
set(ICE_ZLIB_SOURCE_READY_TEST
    "test -f '${ICE_ZLIB_SOURCE_STAMP}' && ! /usr/bin/find '${ICE_ZLIB_SOURCE_DIR}' -type f -newer '${ICE_ZLIB_SOURCE_STAMP}' ! -path '*/.git/*' -print -quit | /usr/bin/grep -q ."
)

string(TOLOWER "${CMAKE_BUILD_TYPE}" ICE_ZLIB_BUILD_TYPE_LOWER)
# 文件名的调试后缀不保证符号被保留，符号策略仍取决于传给子项目的编译参数。 此脚本按单个 CMAKE_BUILD_TYPE
# 选择文件名，不为多配置生成器逐项映射产物。
set(ICE_ZLIB_DEBUG_POSTFIX "")
if(ICE_ZLIB_BUILD_TYPE_LOWER STREQUAL "debug")
  # Debug 后缀只影响 Windows 归档命名，非 Windows 路径仍固定 libz.a。
  set(ICE_ZLIB_DEBUG_POSTFIX "d")
endif()

if(MSVC)
  # MSVC 及采用其 ABI 的驱动使用 .lib，不按编译器品牌猜测归档后缀。
  set(ICE_ZLIB_STATIC_LIBRARY
      "${ICE_ZLIB_LIBRARY_DIR}/zs${ICE_ZLIB_DEBUG_POSTFIX}.lib")
elseif(WIN32)
  # MinGW 使用带 lib 前缀的 COFF 归档，不能复用 MSVC 路径。
  set(ICE_ZLIB_STATIC_LIBRARY
      "${ICE_ZLIB_LIBRARY_DIR}/libzs${ICE_ZLIB_DEBUG_POSTFIX}.a")
else()
  # 非 Windows 的消费路径不带配置后缀，不同配置须隔离构建目录。
  set(ICE_ZLIB_STATIC_LIBRARY "${ICE_ZLIB_LIBRARY_DIR}/libz.a")
endif()

# Ninja 需要获知外部项目生成的库、元数据和戳文件，才能连接依赖图。
set(ICE_ZLIB_BUILD_BYPRODUCTS
    "${ICE_ZLIB_STATIC_LIBRARY}" "${ICE_ZLIB_PKGCONFIG_DIR}/zlib.pc"
    "${ICE_ZLIB_SOURCE_STAMP}")
set(ICE_ZLIB_INSTALL_COMMAND "'${CMAKE_COMMAND}' --build . --target install")
if(WIN32)
  # 兼容文件是同一归档的副本，不新增库实现，也不转换链接类型。
  set(ICE_ZLIB_COMPAT_LIBRARIES)
  if(MSVC)
    # FFmpeg 的 MSVC 探测在不同检查路径会使用 z.lib、zlib.lib 或项目约定的 libz.lib。
    list(APPEND ICE_ZLIB_COMPAT_LIBRARIES "${ICE_ZLIB_LIBRARY_DIR}/libz.lib"
         "${ICE_ZLIB_LIBRARY_DIR}/z.lib" "${ICE_ZLIB_LIBRARY_DIR}/zlib.lib")
    if(NOT ICE_ZLIB_DEBUG_POSTFIX STREQUAL "")
      # 调试归档再补无 d 别名，以兼容只会探测固定文件名的调用方。
      list(APPEND ICE_ZLIB_COMPAT_LIBRARIES "${ICE_ZLIB_LIBRARY_DIR}/libzs.lib")
    endif()
  else()
    # MinGW 的 -lz 探测使用 libz.a，与 zlib 自身的静态库命名不同。
    list(APPEND ICE_ZLIB_COMPAT_LIBRARIES "${ICE_ZLIB_LIBRARY_DIR}/libz.a")
    if(NOT ICE_ZLIB_DEBUG_POSTFIX STREQUAL "")
      list(APPEND ICE_ZLIB_COMPAT_LIBRARIES "${ICE_ZLIB_LIBRARY_DIR}/libzs.a")
    endif()
  endif()

  foreach(compatLibrary IN LISTS ICE_ZLIB_COMPAT_LIBRARIES)
    # 用 && 串接保证任一复制失败都会阻止后续戳更新，下一次仍需重试。
    set(ICE_ZLIB_INSTALL_COMMAND
        "${ICE_ZLIB_INSTALL_COMMAND} && '${CMAKE_COMMAND}' -E copy_if_different '${ICE_ZLIB_STATIC_LIBRARY}' '${compatLibrary}'"
    )
    # 别名也属于生成物，不能仅登记原始库而让下游依赖悬空。
    list(APPEND ICE_ZLIB_BUILD_BYPRODUCTS "${compatLibrary}")
  endforeach()
endif()

ExternalProject_Add(
  zlib_project
  # 本地源码只读参与构建；不设置仓库 URL，更新步骤也显式禁用。
  SOURCE_DIR "${ICE_ZLIB_SOURCE_DIR}"
  BINARY_DIR "${ICE_ZLIB_BINARY_DIR}"
  INSTALL_DIR "${ICE_ZLIB_INSTALL_DIR}"
  UPDATE_COMMAND ""
  # 每次进入自定义检查，但已安装且源码未更新时可跳过实际编译和安装。
  BUILD_ALWAYS TRUE
  # 交叉构建子 CMake 工程时必须显式传入目标系统和工具链，否则 zlib 的检测逻辑会按宿主平台生成并运行测试程序。
  CMAKE_ARGS
    -DCMAKE_INSTALL_PREFIX=${ICE_ZLIB_INSTALL_DIR}
    -DCMAKE_INSTALL_LIBDIR=lib
    # 目标配置与目标平台一起传递，避免在交叉构建中混入宿主探测结果。
    -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
    # 系统名显式传入以启用交叉编译语义，不能仅设置编译器路径就假定平台一致。
    -DCMAKE_SYSTEM_NAME=${CMAKE_SYSTEM_NAME}
    -DCMAKE_SYSTEM_PROCESSOR=${CMAKE_SYSTEM_PROCESSOR}
    # 外部 CMake 不继承父项目缓存，必须显式传递编译驱动及 target triple。
    -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
    -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}
    -DCMAKE_C_COMPILER_TARGET=${CMAKE_C_COMPILER_TARGET}
    -DCMAKE_CXX_COMPILER_TARGET=${CMAKE_CXX_COMPILER_TARGET}
    # sysroot 与归档工具共同约束目标 ABI，不能让子项目自行拾取宿主工具。
    -DCMAKE_SYSROOT=${CMAKE_SYSROOT}
    -DCMAKE_AR=${CMAKE_AR}
    -DCMAKE_RANLIB=${CMAKE_RANLIB}
    # 资源编译与符号工具同样来自父工具链；传入 strip 路径本身不执行剥离。
    -DCMAKE_RC_COMPILER=${CMAKE_RC_COMPILER}
    -DCMAKE_NM=${CMAKE_NM}
    -DCMAKE_STRIP=${CMAKE_STRIP}
    -DCMAKE_OBJCOPY=${CMAKE_OBJCOPY}
    # 配置探针只构建静态库，避免链接目标程序时要求尚不可用的运行环境。
    -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY
    # 保留父级通用 C 参数；调用方不得在这些参数中混入业务模块的 PGO 插桩。
    "-DCMAKE_C_FLAGS=${CMAKE_C_FLAGS}"
    "-DCMAKE_C_FLAGS_DEBUG=${CMAKE_C_FLAGS_DEBUG}"
    "-DCMAKE_C_FLAGS_RELEASE=${CMAKE_C_FLAGS_RELEASE}"
    # 发布含符号配置独立传递，不能用 Release 参数覆盖 RelWithDebInfo。
    "-DCMAKE_C_FLAGS_RELWITHDEBINFO=${CMAKE_C_FLAGS_RELWITHDEBINFO}"
    "-DCMAKE_C_FLAGS_MINSIZEREL=${CMAKE_C_FLAGS_MINSIZEREL}"
    # 静态 zlib 仍需匹配最终程序 CRT，归档类型不等于 MSVC 运行库偏好。
    "-DCMAKE_MSVC_RUNTIME_LIBRARY=${CMAKE_MSVC_RUNTIME_LIBRARY}"
    "-DCMAKE_MSVC_DEBUG_INFORMATION_FORMAT=${CMAKE_MSVC_DEBUG_INFORMATION_FORMAT}"
    # PIC 允许归档进入共享库，不能据此推断本步骤会产出 DLL。
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON
    -DCMAKE_INSTALL_MESSAGE=NEVER
    # 此入口固定构建静态 zlib，当前不依据 ICE_LINKAGE 切换上游库类型。
    -DZLIB_BUILD_SHARED=OFF
    -DZLIB_BUILD_STATIC=ON
    # 只生成依赖所需产物，不把上游测试加入主工程；安装供 FFmpeg 后续探测。
    -DZLIB_BUILD_TESTING=OFF
    -DZLIB_INSTALL=ON
  # 检查只关注源码时间，不验证已安装库是否被删除或编译参数是否已变化。
  BUILD_COMMAND sh -c
                "${ICE_ZLIB_SOURCE_READY_TEST} || '${CMAKE_COMMAND}' --build ."
  # 外层 Ninja 的并行任务数不会显式附加到这里的子 cmake --build 命令。 编译成功后才安装；安装、复制和 touch
  # 作为一个失败可见的命令链执行。
  INSTALL_COMMAND
    sh -c
    "${ICE_ZLIB_SOURCE_READY_TEST} || (${ICE_ZLIB_INSTALL_COMMAND} && '${CMAKE_COMMAND}' -E touch '${ICE_ZLIB_SOURCE_STAMP}')"
  BUILD_BYPRODUCTS ${ICE_ZLIB_BUILD_BYPRODUCTS})
# 上述命令把路径嵌入 POSIX 单引号；目录名含单引号时需要另行完善引用策略。

# 配置期先建立消费目录，实际头和库仍须等待外部项目安装完成。
file(MAKE_DIRECTORY "${ICE_ZLIB_INCLUDE_DIR}")
file(MAKE_DIRECTORY "${ICE_ZLIB_LIBRARY_DIR}")
file(MAKE_DIRECTORY "${ICE_ZLIB_PKGCONFIG_DIR}")
# pkg-config 目录是 FFmpeg 配置阶段的搜索入口，接口库本身不读取其中的 .pc。

add_library(3rd_zlib INTERFACE)
# 包装目标不含源码；链接它只传播头与库，不重新编译另一份压缩实现。 显式构建依赖保证使用接口的目标链接前，外部库及兼容名已就绪。
add_dependencies(3rd_zlib zlib_project)
target_include_directories(3rd_zlib INTERFACE "${ICE_ZLIB_INCLUDE_DIR}")
# 消费真实静态归档路径，不以 FFmpeg 探测别名作为项目链接入口。
target_link_libraries(3rd_zlib INTERFACE "${ICE_ZLIB_STATIC_LIBRARY}")
