# 为 Rubber Band 创建 FFTW 外部构建与稳定消费接口，仅在源码依赖模式加载。

include(ExternalProject)

# 相对于本脚本定位源码；二进制和私有安装前缀置于构建树，不写入上游源码。
set(FFTW_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/../fftw")
set(FFTW_BINARY_DIR "${CMAKE_BINARY_DIR}/3rdpty/fftw_bld")
# 私有前缀不是发布包的按架构/工具链/配置布局，切换工具链应使用独立构建目录。
set(FFTW_INSTALL_DIR "${CMAKE_BINARY_DIR}/3rdpty/fftw_inst")
# 戳文件只有安装成功才更新，未完成安装不能被下一轮认作成功缓存。
set(FFTW_SOURCE_STAMP "${FFTW_INSTALL_DIR}/.ice_fftw_sources.stamp")
# * 缓存只比较源码文件 mtime，不识别删除文件、保留时间的替换或已安装库丢失。
# * 该表达式依赖 POSIX sh、find、grep；Windows 也需从提供这些工具的环境构建。
# * 编译参数变化不是此戳的输入，不能仅凭时间检查通过认定产物已匹配新配置。
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
  # 静态库与动态导入库都使用 .lib 路径，是否还需 DLL 由链接偏好独立决定。
  set(FFTW_LIBRARY "${FFTW_INSTALL_DIR}/lib/fftw3.lib")
  if(FFTW_BUILD_SHARED)
    set(FFTW_RUNTIME_LIBRARY "${FFTW_INSTALL_DIR}/bin/fftw3.dll")
  endif()
elseif(FFTW_BUILD_SHARED)
  # 非 MSVC 动态路径直接使用共享库前后缀，没有单列 MinGW 导入库及 bin 下 DLL。
  set(FFTW_LIBRARY
      "${FFTW_INSTALL_DIR}/lib/${CMAKE_SHARED_LIBRARY_PREFIX}fftw3${CMAKE_SHARED_LIBRARY_SUFFIX}"
  )
else()
  # 只消费双精度 fftw3 归档，此入口没有为 fftw3f 等精度变体创建另一包装目标。
  set(FFTW_LIBRARY "${FFTW_INSTALL_DIR}/lib/libfftw3.a")
endif()

set(FFTW_ENABLE_THREADS ON)
# 开启线程子库构建不等于把它链接给消费方，下方接口仍只列 FFTW_LIBRARY。
if(MSVC)
  # MSVC 原生环境没有 pthread.h，禁用 FFTW threads 子库以避免误启用 pthread 后端。
  set(FFTW_ENABLE_THREADS OFF)
endif()

set(FFTW_C_FLAGS "${CMAKE_C_FLAGS}")
# 父级通用 C 参数会传入外部项目，调用方不得将业务 PGO 插桩混入其中。
if(MSVC)
  # 只追加类型转换诊断屏蔽，不整体关闭第三方编译警告。
  string(APPEND FFTW_C_FLAGS " /wd4244")
  if(CMAKE_MSVC_RUNTIME_LIBRARY)
    # 优先继承父级 CRT 偏好，确保依赖和最终消费者使用相同运行库模型。
    set(FFTW_MSVC_RUNTIME_LIBRARY "${CMAKE_MSVC_RUNTIME_LIBRARY}")
  elseif(CMAKE_BUILD_TYPE MATCHES Debug)
    # 未提供 CRT 偏好时默认静态运行库；此后备不单独依据 ICE_LINKAGE 推导。
    set(FFTW_MSVC_RUNTIME_LIBRARY "MultiThreadedDebug")
  else()
    set(FFTW_MSVC_RUNTIME_LIBRARY "MultiThreaded")
  endif()
  if(FFTW_MSVC_RUNTIME_LIBRARY MATCHES "DLL")
    # DLL 指 CRT 类型，不是 FFTW 自身的库类型；两组开关不能互相替代。
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
      # 含符号发布配置保留 /Zi，不能把普通 Release 的参数直接复用于此配置。
      "-DCMAKE_C_FLAGS_RELWITHDEBINFO=${FFTW_MSVC_RUNTIME_FLAG_RELEASE} /Zi /O2 /Ob1 /DNDEBUG"
      "-DCMAKE_C_FLAGS_MINSIZEREL=${FFTW_MSVC_RUNTIME_FLAG_RELEASE} /O1 /Ob1 /DNDEBUG"
  )
endif()
# 只去掉首尾空白，不改写参数内部的空格或用户传入的引号。
string(STRIP "${FFTW_C_FLAGS}" FFTW_C_FLAGS)

# 使用 ExternalProject 构建 FFTW，避免修改源码或直接处理其复杂的 CMakeLists。 传入
# CMAKE_POLICY_VERSION_MINIMUM=3.5，绕过旧 cmake_minimum_required 带来的致命错误。 强制
# CMAKE_INSTALL_LIBDIR 为 "lib"，避免部分 Linux 发行版使用 "lib64"。
ExternalProject_Add(
  fftw_project
  # 只使用工作区现有源码，不配置下载或版本控制更新步骤。
  SOURCE_DIR "${FFTW_SOURCE_DIR}"
  BINARY_DIR "${FFTW_BINARY_DIR}"
  INSTALL_DIR "${FFTW_INSTALL_DIR}"
  UPDATE_COMMAND ""
  # 每次进入自定义增量检查，但不代表每次都会重新编译或安装。
  BUILD_ALWAYS TRUE
  # FFTW 的旧 CMake 工程不会自动继承外层交叉工具链；这里显式传入目标系统和 try-compile 类型，避免宿主 Linux 链接参数污染
  # MinGW 构建。
  CMAKE_ARGS -DCMAKE_INSTALL_PREFIX=${FFTW_INSTALL_DIR}
             # 固定 lib，保证后面写死的链接路径不受宿主 lib64 默认安装约定影响。
             -DCMAKE_INSTALL_LIBDIR=lib
             # 仅调整旧项目的策略兼容下限，不修改上游 cmake_minimum_required 源文件。
             -DCMAKE_POLICY_VERSION_MINIMUM=3.5
             -DBUILD_SHARED_LIBS=${FFTW_BUILD_SHARED}
             # 禁用上游测试不替代引擎数值与实时测试，依赖构建只负责生成库。
             -DBUILD_TESTS=OFF
             -DENABLE_THREADS=${FFTW_ENABLE_THREADS}
             # 子项目按单个 CMAKE_BUILD_TYPE 配置，不在此为多配置逐项建立导入映射。
             -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
             -DCMAKE_SYSTEM_NAME=${CMAKE_SYSTEM_NAME}
             -DCMAKE_SYSTEM_PROCESSOR=${CMAKE_SYSTEM_PROCESSOR}
             # 外部配置不自动继承父缓存，驱动与 target triple 都显式传递。
             -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
             -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}
             -DCMAKE_C_COMPILER_TARGET=${CMAKE_C_COMPILER_TARGET}
             -DCMAKE_CXX_COMPILER_TARGET=${CMAKE_CXX_COMPILER_TARGET}
             # sysroot 和归档工具必须来自目标工具链，不能与宿主默认工具混用。
             -DCMAKE_SYSROOT=${CMAKE_SYSROOT}
             -DCMAKE_AR=${CMAKE_AR}
             -DCMAKE_RANLIB=${CMAKE_RANLIB}
             # Windows 资源编译及符号工具同样透传；提供 strip 路径本身不执行剥离。
             -DCMAKE_RC_COMPILER=${CMAKE_RC_COMPILER}
             -DCMAKE_NM=${CMAKE_NM}
             -DCMAKE_STRIP=${CMAKE_STRIP}
             -DCMAKE_OBJCOPY=${CMAKE_OBJCOPY}
             # 配置探针只构建静态库，避免要求交叉目标的完整程序链接环境。
             -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY
             "-DCMAKE_C_FLAGS=${FFTW_C_FLAGS}"
             # 配置专属 flags 在此只追加 MSVC 参数，非 MSVC 不显式透传父级各配置 flags。
             ${FFTW_MSVC_RUNTIME_ARGS}
             # 静态归档也启用 PIC，允许后续进入共享库；不会改变 BUILD_SHARED_LIBS。
             -DCMAKE_POSITION_INDEPENDENT_CODE=ON
             -DCMAKE_INSTALL_MESSAGE=NEVER
  # 外层并行度没有显式传入这条子构建命令；源码戳有效时整个命令被短路。
  BUILD_COMMAND sh -c
                "${FFTW_SOURCE_READY_TEST} || '${CMAKE_COMMAND}' --build ."
  INSTALL_COMMAND
    sh -c
    "${FFTW_SOURCE_READY_TEST} || ('${CMAKE_COMMAND}' --build . --target install && '${CMAKE_COMMAND}' -E touch '${FFTW_SOURCE_STAMP}')"
  # Ninja 需要知道外部生成库和戳的归属；线程子库或符号文件没有在此单独登记。
  BUILD_BYPRODUCTS "${FFTW_LIBRARY}" ${FFTW_RUNTIME_LIBRARY}
                   "${FFTW_SOURCE_STAMP}")

# 预创建 include 目录，避免 CMake 配置阶段报错。
file(MAKE_DIRECTORY "${FFTW_INSTALL_DIR}/include")
# 配置期存在的空 include 目录不表示头已安装，真实内容仍由外部项目安装步骤提供。

add_library(3rd_fftw3 INTERFACE)
# 显式构建依赖把外部安装接入目标图，避免消费者在库尚未就绪时开始链接。
add_dependencies(3rd_fftw3 fftw_project)

# 只暴露私有安装后的公共头，不让消费者依赖外部项目中间构建路径。
target_include_directories(3rd_fftw3 INTERFACE "${FFTW_INSTALL_DIR}/include")

# 包装名与预编译模式一致，不在业务侧暴露 ExternalProject 的目标名或安装细节。
target_link_libraries(3rd_fftw3 INTERFACE "${FFTW_LIBRARY}")
