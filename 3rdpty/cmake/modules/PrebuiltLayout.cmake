include_guard(GLOBAL)
# 守卫只防止重复定义 helper，不阻止不同 Find 模块多次调用初始化或查询函数。

# 本文件只解析预编译布局与文件路径，具体目标必须由各自 Find 模块显式创建。 初始化写缓存供各模块共享，运行时收集使用全局属性；这些都只在配置期执行。

# * 生成编译器标签候选，以当前主版本为首项，再按整数主版本递减。
# * out_var 是调用者接收列表的变量名；tag_prefix 是 gcc、clang 等目录前缀。
# * compiler_major 仅接受十进制数字，空值或无法解析的版本退化为无版本前缀。
# * 候选表示目录查找顺序，不检验 ABI、标准库或编译选项是否真的向后兼容。
# * 例如前缀 clang 与主版本 3 得到 clang3、clang2、clang1，不追加 gcc 或 clang64。
function(ice_prebuilt_compiler_tag_fallback_candidates out_var tag_prefix
         compiler_major)
  set(_candidates "")
  # 从空局部列表重建，不能把调用作用域遗留的候选串接进来。
  if(compiler_major MATCHES "^[0-9]+$")
    # 不通过真实文件枚举构造列表，尚不存在的版本目录由后续选择器略过。
    set(_candidate_major "${compiler_major}")
    while(_candidate_major GREATER 0)
      list(APPEND _candidates "${tag_prefix}${_candidate_major}")
      # 下界停在 1，避免产生 tag0 或负版本目录；不同编译器族之间不会回退。
      math(EXPR _candidate_major "${_candidate_major} - 1")
    endwhile()
  endif()
  if(_candidates STREQUAL "")
    # 无法解析版本不立即报错，仍允许使用明确命名的无版本预编译包。
    set(_candidates "${tag_prefix}")
  endif()
  set(${out_var}
      ${_candidates}
      PARENT_SCOPE)
endfunction()

# * 初始化共享缓存中的链接偏好、包根、平台、架构、工具链与编译器标签。
# * default_root 仅用于未提供 ICE_PREBUILT_ROOT 的调用；已有非空缓存优先。
# * 公共头布局为 headers/<包>/include，二进制按平台、包、架构及工具链隔离。
# * 静态布局为 libs/<arch>/<toolchain>/<compiler-tag>/<config>。
# * 动态库和导入库分别位于 bin 与 libs，标签之后多一层 shared/<config>。
# * 这里只检查平台二进制根存在，各包、头、库的完整性留到具体查找时验证。
# * 配置失败直接中止，不隐式下载或切换到源码依赖模式。
# * 默认值缓存后不会因换编译器自动全部刷新，切换工具链应使用独立构建目录。
# * 显式平台和架构等覆盖值按路径片段直接使用，仅链接偏好在此做枚举校验。
function(ice_prebuilt_init default_root)
  if(NOT DEFINED ICE_LINKAGE OR ICE_LINKAGE STREQUAL "")
    # 独立入口默认静态；嵌入时由上层先设置继承的偏好，不在这里引用主项目变量。
    set(ICE_LINKAGE
        "static"
        CACHE STRING "Engine dependency linkage preference.")
  endif()
  string(TOLOWER "${ICE_LINKAGE}" ICE_LINKAGE)
  # 规范化后只接受两种约定值，不能让拼写错误静默落入静态路径。
  if(NOT ICE_LINKAGE MATCHES "^(static|shared)$")
    message(FATAL_ERROR "ICE_LINKAGE 只能是 static 或 shared。")
  endif()
  # FORCE 仅用于规范化后的链接值，确保后续 Find 模块读到相同大小写。
  set(ICE_LINKAGE
      "${ICE_LINKAGE}"
      CACHE STRING "Engine dependency linkage preference." FORCE)

  if(NOT DEFINED ICE_PREBUILT_ROOT OR ICE_PREBUILT_ROOT STREQUAL "")
    # 缓存保存选定根目录；后续 include 传入另一默认根不会覆盖已选位置。
    set(ICE_PREBUILT_ROOT
        "${default_root}"
        CACHE PATH "Root directory containing engine prebuilt packages.")
  endif()

  # 平台目录名固定为仓库约定，避免直接暴露 CMake 系统名。
  if(NOT DEFINED ICE_PREBUILT_PLATFORM OR ICE_PREBUILT_PLATFORM STREQUAL "")
    # 根据目标平台条件而非 CMAKE_HOST_SYSTEM_NAME 选择，支持宿主与目标不同的构建。
    if(WIN32)
      set(_platform "windows")
    elseif(APPLE)
      set(_platform "macos")
    else()
      # 未单独识别的系统一律映射 linux，新平台必须显式覆盖而非依赖自动识别。
      set(_platform "linux")
    endif()
    set(ICE_PREBUILT_PLATFORM
        # 目录名由包布局约定，Windows 大小写和 CMake 系统名并不直接对应。
        "${_platform}"
        CACHE STRING "Prebuilt platform directory name.")
  endif()

  # macOS 默认使用 Apple Silicon 预编译布局；其它平台继续按指针宽度选择。 交叉构建或 Intel macOS 构建可显式设置
  # ICE_PREBUILT_ARCH 覆盖默认值。
  if(NOT DEFINED ICE_PREBUILT_ARCH OR ICE_PREBUILT_ARCH STREQUAL "")
    if(APPLE)
      set(_arch "arm64")
    elseif(CMAKE_SIZEOF_VOID_P EQUAL 8)
      # 指针宽度不区分 x86_64 与其他 64 位 ISA，非 Apple ARM 目标须显式设架构。
      set(_arch "x86_64")
    else()
      set(_arch "x86")
    endif()
    set(ICE_PREBUILT_ARCH
        # 此缓存只是选包键，不会更改编译器实际生成的机器指令架构。
        "${_arch}"
        CACHE STRING "Prebuilt architecture directory name.")
  endif()

  # 工具链目录用于区分 MSVC、MinGW 等二进制格式。
  if(NOT DEFINED ICE_PREBUILT_TOOLCHAIN OR ICE_PREBUILT_TOOLCHAIN STREQUAL "")
    if(MSVC)
      # 优先区分 MSVC ABI，避免 clang-cl 因编译器 ID 为 Clang 被归入普通 clang。
      set(_toolchain "msvc")
    elseif(MINGW)
      # 归入同一 mingw 工具链层后，Clang 与 GCC 的标准库区别仍由下一层标签表达。
      set(_toolchain "mingw")
    elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
      set(_toolchain "gcc")
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
      set(_toolchain "clang")
    else()
      # 保留未知驱动的可覆盖目录名，不据此承诺已有该工具链的预编译产物。
      string(TOLOWER "${CMAKE_CXX_COMPILER_ID}" _toolchain)
    endif()
    set(ICE_PREBUILT_TOOLCHAIN
        # 包搜索只使用该层级名，不负责安装或切换当前编译驱动。
        "${_toolchain}"
        CACHE STRING "Prebuilt toolchain directory name.")
  endif()

  # 编译器标签用于区分同一工具链下的 ABI 或运行库版本。
  if(NOT DEFINED ICE_PREBUILT_COMPILER_TAG OR ICE_PREBUILT_COMPILER_TAG
                                              STREQUAL "")
    if(MSVC)
      # 2026 是仓库布局标签而非从 MSVC_VERSION 计算的值，包升级需同步维护约定。
      set(_compiler_tag "2026")
    elseif(MINGW AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
      # Windows MinGW Clang 使用 CLANG64 布局；其 C runtime 是 UCRT，但 C++ runtime 是
      # libc++。
      set(_compiler_tag "clang64")
    elseif(MINGW)
      # 默认假设 UCRT64；其他 MinGW 运行库或独立发行版需显式覆盖标签。
      set(_compiler_tag "ucrt64")
    elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
      # Linux GCC 预编译库按主版本区分 ABI 和 libstdc++ 版本，例如 gcc14。
      string(REGEX MATCH "^[0-9]+" _compiler_major
                   "${CMAKE_CXX_COMPILER_VERSION}")
      if(_compiler_major)
        set(_compiler_tag "gcc${_compiler_major}")
        # 较旧版本只是可尝试的包，不会检查 libstdc++ 符号或构建参数的兼容性。
        ice_prebuilt_compiler_tag_fallback_candidates(
          _compiler_tag_candidates "gcc" "${_compiler_major}")
      else()
        # 无主版本信息时精确使用 gcc 目录，不构造任意版本范围。
        set(_compiler_tag "gcc")
        set(_compiler_tag_candidates "${_compiler_tag}")
      endif()
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
      # Linux Clang 预编译库按主版本区分前端与标准库组合，例如 clang19。
      string(REGEX MATCH "^[0-9]+" _compiler_major
                   "${CMAKE_CXX_COMPILER_VERSION}")
      if(_compiler_major)
        set(_compiler_tag "clang${_compiler_major}")
        # 同名 Clang 主版本也可能搭配不同标准库，目录命中不能替代链接验证。
        ice_prebuilt_compiler_tag_fallback_candidates(
          _compiler_tag_candidates "clang" "${_compiler_major}")
      else()
        # AppleClang 也满足 Clang 匹配，使用专门包标签时需显式覆盖默认值。
        set(_compiler_tag "clang")
        set(_compiler_tag_candidates "${_compiler_tag}")
      endif()
    else()
      string(TOLOWER "${CMAKE_CXX_COMPILER_ID}" _compiler_tag)
      set(_compiler_tag_candidates "${_compiler_tag}")
    endif()
    if(NOT DEFINED _compiler_tag_candidates)
      # MSVC 与 MinGW 固定标签不做版本递减，候选必须至少保留当前选定标签。
      set(_compiler_tag_candidates "${_compiler_tag}")
    endif()
    set(ICE_PREBUILT_COMPILER_TAG
        # 主标签与回退列表分别存储，日志可同时显示请求标签和实际尝试目录。
        "${_compiler_tag}"
        CACHE STRING "Prebuilt compiler/runtime directory name.")
  else()
    # 显式或缓存标签通常精确匹配；仅恰好等于当前版本自动标签时重建版本回退。
    set(_compiler_tag_candidates "${ICE_PREBUILT_COMPILER_TAG}")
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
      string(REGEX MATCH "^[0-9]+" _compiler_major
                   "${CMAKE_CXX_COMPILER_VERSION}")
      if(_compiler_major AND ICE_PREBUILT_COMPILER_TAG STREQUAL
                             "gcc${_compiler_major}")
        # 因此显式传入当前 gccN 不能用来禁止旧版本回退，需选择独立包标签。
        ice_prebuilt_compiler_tag_fallback_candidates(
          _compiler_tag_candidates "gcc" "${_compiler_major}")
      endif()
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
      string(REGEX MATCH "^[0-9]+" _compiler_major
                   "${CMAKE_CXX_COMPILER_VERSION}")
      if(_compiler_major AND ICE_PREBUILT_COMPILER_TAG STREQUAL
                             "clang${_compiler_major}")
        # clang64 等运行库标签不满足此条件，仍是单一目录候选。
        ice_prebuilt_compiler_tag_fallback_candidates(
          _compiler_tag_candidates "clang" "${_compiler_major}")
      endif()
    endif()
  endif()
  # 候选为派生状态，每次初始化重写，防止标签改变后保留旧列表。
  set(ICE_PREBUILT_COMPILER_TAG_CANDIDATES
      "${_compiler_tag_candidates}"
      CACHE INTERNAL "Prebuilt compiler/runtime directory fallback names."
            FORCE)

  set(_platform_root "${ICE_PREBUILT_ROOT}/binaries/${ICE_PREBUILT_PLATFORM}")
  if(NOT IS_DIRECTORY "${_platform_root}")
    # 缺平台根即失败；错误提示里的源码开关是用户选择，不是自动回退行为。
    message(
      FATAL_ERROR
        "SOURCES_BUILD=OFF 需要引擎预编译库目录：${_platform_root}。请提供预编译库，或显式开启 SOURCES_BUILD。"
    )
  endif()
endfunction()

# * 读取初始化产生的编译器标签候选，结果通过 out_var 返回调用作用域。
# * 若没有候选缓存则使用单一标签；两者都不存在时返回空列表而非自动初始化。
# * 调用者必须先初始化布局，否则目录选择将没有可遍历的标签。
# * 此函数保持既有候选顺序，不再排序或探测文件系统。
# * 返回值是 CMake 列表而不是单一路径，消费者必须逐项遍历，不能直接拼接为目录。
function(ice_prebuilt_compiler_tag_candidates out_var)
  if(DEFINED ICE_PREBUILT_COMPILER_TAG_CANDIDATES
     AND NOT ICE_PREBUILT_COMPILER_TAG_CANDIDATES STREQUAL "")
    # 初始化计算的派生列表优先，保证各 Find 模块对同一标签采用一致回退策略。
    set(_candidates ${ICE_PREBUILT_COMPILER_TAG_CANDIDATES})
  elseif(DEFINED ICE_PREBUILT_COMPILER_TAG)
    # 兼容仅给出精确标签的调用环境，不在读取阶段扩大查找范围。
    set(_candidates "${ICE_PREBUILT_COMPILER_TAG}")
  else()
    set(_candidates "")
  endif()
  set(${out_var}
      ${_candidates}
      PARENT_SCOPE)
endfunction()

# * 返回 binaries/<platform>/<package> 并要求它已是实际目录。
# * out_var 为返回路径的变量名；package 必须是调用模块给出的受信任包名。
# * 不扫描包内二进制，也不校验路径片段是否包含上级目录或绝对路径。
# * 缺包直接终止配置，保证预编译模式不以系统安装或源码构建补位。
# * 返回路径保持输入根的写法，不执行 REAL_PATH 规范化或符号链接解析。
function(ice_prebuilt_package_root out_var package)
  set(_root "${ICE_PREBUILT_ROOT}/binaries/${ICE_PREBUILT_PLATFORM}/${package}")
  if(NOT IS_DIRECTORY "${_root}")
    # 头目录存在并不足以证明目标平台提供了该包，二进制根必须独立检查。
    message(FATAL_ERROR "缺少引擎预编译包目录：${_root}")
  endif()
  set(${out_var}
      "${_root}"
      PARENT_SCOPE)
endfunction()

# * 返回 headers/<package>/include，头目录不区分平台或构建配置。
# * 该检查只验证目录存在，不验证特定头文件、库版本或头与二进制的 ABI 配对。
# * out_var 接收路径；具体目标负责将它写入包含目录使用要求。
# * 此函数不依赖包二进制目录，用于各 Find 模块在创建目标前验证共享头布局。
# * 包名应与打包目录逐字一致，不能根据公开 target 的命名空间推断包名。
function(ice_prebuilt_include_dir out_var package)
  set(_include_dir "${ICE_PREBUILT_ROOT}/headers/${package}/include")
  if(NOT IS_DIRECTORY "${_include_dir}")
    # 不借用系统 include 目录掩盖缺包，否则头与预编译库可能来自不同版本。
    message(FATAL_ERROR "缺少引擎预编译头文件目录：${_include_dir}")
  endif()
  set(${out_var}
      "${_include_dir}"
      PARENT_SCOPE)
endfunction()

# * 返回当前生成器需要导入的配置名，保留生成器提供的顺序与大小写。
# * 多配置返回整个配置列表，单配置返回 CMAKE_BUILD_TYPE。
# * 两者都未设置时按 Release 查包，但不更改用户的 CMAKE_BUILD_TYPE 缓存。
# * Find 模块仍需为无构建类型的消费补充 NOCONFIG 导入属性。
# * 返回列表不会主动增添 Debug，是否需要该配置取决于生成器请求。
function(ice_prebuilt_target_configs out_var)
  if(CMAKE_CONFIGURATION_TYPES)
    # 不只处理当前默认配置，多配置工程在同一配置阶段需要完整的导入位置。
    set(_configs ${CMAKE_CONFIGURATION_TYPES})
  elseif(CMAKE_BUILD_TYPE)
    # 自定义单配置名也原样保留，由磁盘候选映射处理，而非在此限制为标准四配置。
    set(_configs "${CMAKE_BUILD_TYPE}")
  else()
    set(_configs "Release")
  endif()
  set(${out_var}
      ${_configs}
      PARENT_SCOPE)
endfunction()

# * 将请求配置转换为磁盘目录候选，不更改目标上的请求配置名称。
# * Debug 仅允许调试目录；发布型配置优先含符号目录，再回退到 Release。
# * 未知配置先尝试原名及小写名，再尝试标准发布目录，不能据此保证相同编译选项。
# * 候选去重保持首项优先，适用于大小写敏感文件系统及历史小写打包布局。
# * 这里只改变依赖包的选择，不修改主目标优化级别、断言或调试信息生成参数。
function(ice_prebuilt_config_dir_candidates out_var config)
  string(TOLOWER "${config}" _config_lower)
  if(_config_lower STREQUAL "debug")
    # 不以发布库补齐缺失的 Debug，避免混用调试 ABI 与 MSVC 运行库。
    set(_candidates "Debug" "debug")
  elseif(config STREQUAL "" OR _config_lower STREQUAL "noconfig")
    # 没有配置属性的导入也复用发布包，不要求预编译仓额外放一个 NOCONFIG 目录。
    set(_candidates "RelWithDebInfo" "relwithdebinfo" "Release" "release")
  elseif(
    _config_lower STREQUAL "release"
    OR _config_lower STREQUAL "relwithdebinfo"
    OR _config_lower STREQUAL "minsizerel")
    # 即使请求 Release 也先用 RelWithDebInfo；目录选择不剥离已有调试信息。
    set(_candidates "RelWithDebInfo" "relwithdebinfo" "Release" "release")
  else()
    set(_candidates "${config}" "${_config_lower}" "RelWithDebInfo"
                    "relwithdebinfo" "Release" "release")
  endif()
  # 原名已经是小写时可能与第二项重复，去重可避免重复文件系统查询及噪声日志。
  list(REMOVE_DUPLICATES _candidates)
  set(${out_var}
      ${_candidates}
      PARENT_SCOPE)
endfunction()

# * 在 tagged_base_dir/<compiler-tag><tagged_suffix>/<config> 中选首个存在目录。
# * tagged_suffix 为静态空串或动态 /shared；config 是尚未映射的请求配置名。
# * out_var 返回路径，out_var_SEARCHED_DIRS 返回实际尝试过的目录，失败时路径为空。
# * 先遍历标签再遍历配置，因此新标签的 Release 可优先于旧标签的 RelWithDebInfo。
# * 这里只判断目录存在，不判断内部库文件齐全；调用者找库失败不会回来继续回退。
# * 选择器不创建缺失目录，避免将空壳目录误认为打包完成的产物。
# * tagged_suffix 含路径分隔符，调用方不得同时在 base 尾部再拼编译器标签。
function(ice_prebuilt_select_config_dir out_var tagged_base_dir tagged_suffix
         config)
  ice_prebuilt_compiler_tag_candidates(_compiler_tag_candidates)
  ice_prebuilt_config_dir_candidates(_config_candidates "${config}")
  set(_searched_dirs "")
  foreach(_compiler_tag IN LISTS _compiler_tag_candidates)
    # 标签顺序即兼容优先级，不按目录时间、内容或版本号另做排序。
    if(_compiler_tag STREQUAL "")
      # 保留无标签布局的拼接分支；空候选列表本身不会进入本循环。
      set(_config_base_dir "${tagged_base_dir}${tagged_suffix}")
    else()
      set(_config_base_dir
          "${tagged_base_dir}/${_compiler_tag}${tagged_suffix}")
    endif()
    foreach(_config_dir_name IN LISTS _config_candidates)
      set(_candidate_dir "${_config_base_dir}/${_config_dir_name}")
      # 失败日志保留每个实际尝试路径，便于区分标签、配置与架构布局错误。
      list(APPEND _searched_dirs "${_candidate_dir}")
      if(IS_DIRECTORY "${_candidate_dir}")
        set(${out_var}
            "${_candidate_dir}"
            PARENT_SCOPE)
        set(${out_var}_SEARCHED_DIRS
            ${_searched_dirs}
            PARENT_SCOPE)
        # 提前返回的诊断列表只到命中项，不包含尚未检查的后续候选。
        return()
      endif()
    endforeach()
  endforeach()

  # 失败也显式清空输出，避免调用者误用上一包或上一配置的有效路径。
  set(${out_var}
      ""
      PARENT_SCOPE)
  set(${out_var}_SEARCHED_DIRS
      ${_searched_dirs}
      PARENT_SCOPE)
endfunction()

# * 解析指定包的链接库目录，依 ICE_LINKAGE 选择静态或 shared 层级。
# * out_var 返回目录而非具体库文件；config 保留调用方请求供错误诊断使用。
# * 静态与动态之间不互相回退，缺任一必需目录即终止配置。
# * 库目录命中不证明里面的归档架构、链接类型或符号格式正确。
# * static 布局不含额外 static 层；擅自增加该层会使所有既有静态包查找失败。
function(ice_prebuilt_library_dir out_var package config)
  ice_prebuilt_package_root(_package_root "${package}")
  set(_library_tagged_base_dir
      "${_package_root}/libs/${ICE_PREBUILT_ARCH}/${ICE_PREBUILT_TOOLCHAIN}")
  set(_library_tagged_suffix "")
  if(ICE_LINKAGE STREQUAL "shared")
    # Windows shared 目录放导入库，真正 DLL 由独立 runtime 目录解析。
    set(_library_tagged_suffix "/shared")
  endif()

  ice_prebuilt_select_config_dir(_library_dir "${_library_tagged_base_dir}"
                                 "${_library_tagged_suffix}" "${config}")
  if(_library_dir)
    # 返回选择器确认存在的目录；具体文件名交给每个包自己的 Find 模块提供。
    set(${out_var}
        "${_library_dir}"
        PARENT_SCOPE)
    return()
  endif()

  message(
    FATAL_ERROR
      "缺少 ${package} 的 ${config} 引擎预编译库目录。编译器标签候选：${ICE_PREBUILT_COMPILER_TAG_CANDIDATES}；已尝试：${_library_dir_SEARCHED_DIRS}"
  )
endfunction()

# * 解析 bin/.../shared 下的运行时目录，缺目录时直接报错。
# * 仅动态 Windows 库查找需要调用；本函数本身不会检查当前链接偏好或平台。
# * out_var 接收目录，config 使用与链接库相同的候选规则独立解析。
# * 独立查找可能命中不同标签或回退配置，不能仅凭两者存在断言 DLL 与导入库匹配。
# * 运行时目录的返回路径不设置 PATH，也不修改目标的 RPATH 或加载器行为。
function(ice_prebuilt_runtime_dir out_var package config)
  ice_prebuilt_package_root(_package_root "${package}")
  set(_runtime_tagged_base_dir
      "${_package_root}/bin/${ICE_PREBUILT_ARCH}/${ICE_PREBUILT_TOOLCHAIN}")
  # runtime 永远走 shared 层，不会误把静态库或符号目录作为可部署文件来源。
  ice_prebuilt_select_config_dir(_runtime_dir "${_runtime_tagged_base_dir}"
                                 "/shared" "${config}")
  if(_runtime_dir)
    # 找到目录只完成布局查询，具体 DLL 是否存在必须由后续文件级检查确认。
    set(${out_var}
        "${_runtime_dir}"
        PARENT_SCOPE)
    return()
  endif()

  message(
    FATAL_ERROR
      "缺少 ${package} 的 ${config} 引擎动态运行时目录。编译器标签候选：${ICE_PREBUILT_COMPILER_TAG_CANDIDATES}；已尝试：${_runtime_dir_SEARCHED_DIRS}"
  )
endfunction()

# * 解析独立 symbols 目录，链接偏好与配置映射遵循库目录约定。
# * package 的二进制根仍是必需项；只有符号子目录允许缺失并返回空路径。
# * out_var 只返回目录，不判断符号格式，也不关联符号与某一个具体库文件。
# * 这是现有可选查找行为，不代表 Debug 或含符号发布包缺失 PDB 已满足打包规范。
# * 符号层独立于 libs 与 bin；不能把导入库或 DLL 本身当成旁路符号文件。
function(ice_prebuilt_symbol_dir out_var package config)
  ice_prebuilt_package_root(_package_root "${package}")
  set(_symbol_tagged_base_dir
      "${_package_root}/symbols/${ICE_PREBUILT_ARCH}/${ICE_PREBUILT_TOOLCHAIN}")
  set(_symbol_tagged_suffix "")
  if(ICE_LINKAGE STREQUAL "shared")
    # 动态包的符号与静态包同名也保持目录隔离，避免同配置下错误混用 PDB。
    set(_symbol_tagged_suffix "/shared")
  endif()

  ice_prebuilt_select_config_dir(_symbol_dir "${_symbol_tagged_base_dir}"
                                 "${_symbol_tagged_suffix}" "${config}")
  if(_symbol_dir)
    # 符号不是链接输入，找到目录也不将它加入任何目标依赖或链接参数。
    set(${out_var}
        "${_symbol_dir}"
        PARENT_SCOPE)
    return()
  endif()

  set(${out_var}
      ""
      PARENT_SCOPE)
endfunction()

# * 收集指定包与配置符号目录中的所有顶层 .pdb 文件，结果以列表返回 out_var。
# * 目录或匹配文件不存在时返回空列表，不作为缺包错误。
# * 只枚举 PDB，不检查 MinGW 归档内的 CodeView 或 ELF 归档内的 DWARF。
# * 结果没有按库名匹配，也不验证 PDB 标识与二进制一致；打包验收需单独完成。
# * 不递归遍历目录，符号应直接放在选定配置目录，额外子目录不会被部署方获知。
function(ice_prebuilt_find_symbol_files out_var package config)
  ice_prebuilt_symbol_dir(_symbol_dir "${package}" "${config}")
  if(_symbol_dir STREQUAL "")
    # 没有符号目录时不做 glob，避免把空路径解释成当前目录。
    set(${out_var}
        ""
        PARENT_SCOPE)
    return()
  endif()

  # CONFIGURE_DEPENDS 追踪匹配集合变化，不会复制、生成或修复符号文件。
  file(GLOB _symbol_files CONFIGURE_DEPENDS "${_symbol_dir}/*.pdb")
  set(${out_var}
      ${_symbol_files}
      PARENT_SCOPE)
endfunction()

# * 将已解析的 runtime_file 登记到全局 ICE_PREBUILT_RUNTIME_FILES 列表。
# * 仅 shared/Windows 且路径非空时登记，其他情况为无操作。
# * 调用方负责检查文件存在；这里按完整路径去重，不检查 DLL 名称或依赖闭包。
# * 列表不按配置分组，多配置查找结果会同时进入同一全局集合。
# * 登记只改变本次 CMake 进程的属性，不把列表持久化到用户缓存或磁盘清单。
function(ice_prebuilt_record_runtime_file runtime_file)
  # 列表以分号分隔，包路径含分号时不能当作普通单一路径安全登记。
  if(NOT ICE_LINKAGE STREQUAL "shared"
     OR NOT WIN32
     OR runtime_file STREQUAL "")
    return()
  endif()

  get_property(_runtime_files GLOBAL PROPERTY ICE_PREBUILT_RUNTIME_FILES)
  # 多个 Find 模块可能解析出同一个 DLL，重复路径只保留首次登记的位置。
  list(APPEND _runtime_files "${runtime_file}")
  list(REMOVE_DUPLICATES _runtime_files)
  # 去重依据路径字符串，不解析同名文件内容；不同目录中的同名 DLL 都会留下。
  set_property(GLOBAL PROPERTY ICE_PREBUILT_RUNTIME_FILES "${_runtime_files}")
endfunction()

# * 读取本次 CMake 配置过程中累计登记的运行时路径，并通过 out_var 返回。
# * 结果可能同时包含 Debug 与发布目录，不是构建时 $<CONFIG> 筛选后的列表。
# * 未登记时返回空值；只读此函数不会补做包查找、路径验证或部署。
# * 返回前再次去重，以兼容其他维护脚本直接写入全局属性的情况。
# * 接收变量不获得全局属性的引用，之后的登记不会自动更新已经返回的局部列表。
function(ice_prebuilt_get_runtime_files out_var)
  get_property(_runtime_files GLOBAL PROPERTY ICE_PREBUILT_RUNTIME_FILES)
  if(_runtime_files)
    list(REMOVE_DUPLICATES _runtime_files)
  endif()
  set(${out_var}
      ${_runtime_files}
      PARENT_SCOPE)
endfunction()

# * 为已经存在的 target_name 附加构建后复制，将当前登记 DLL 放在目标文件旁。
# * 调用位置必须满足 TARGET 形式 add_custom_command 的目录约束，并在各包登记之后。
# * 仅处理 Windows shared；无登记文件时不添加空命令。
# * 该部署只覆盖显式登记项，不递归扫描 DLL 依赖，不能保证任意机器都可直接运行。
# * 多配置同名 DLL 可互相覆盖，当前列表未按目标配置过滤，调用者需留意这一局限。
# * 此函数不自行检查目标存在性或目标种类，无效调用由 CMake 生成命令时报错。
# * 复制失败由构建工具报告，不执行删除旧 DLL、回滚或自动重试。
function(ice_prebuilt_copy_runtime_files target_name)
  # 应在同一目标上集中调用一次，重复调用会附加多份 POST_BUILD 复制命令。
  if(NOT ICE_LINKAGE STREQUAL "shared" OR NOT WIN32)
    return()
  endif()

  ice_prebuilt_get_runtime_files(_runtime_files)
  if(NOT _runtime_files)
    # 此次调用不会跟踪将来新增的登记项，空列表时需要调用方调整配置顺序。
    return()
  endif()

  add_custom_command(
    TARGET ${target_name}
    # 目标完成构建才执行复制；单独替换源 DLL 不保证触发一次新的 POST_BUILD。
    POST_BUILD
    # copy_if_different 避免无谓改写目标时间；此处不建立安装规则或运行时搜索路径。
    COMMAND
      ${CMAKE_COMMAND} -E copy_if_different ${_runtime_files}
      # 生成器表达式按实际目标配置解析输出位置，不硬编码 Debug/Release 子目录。
      "$<TARGET_FILE_DIR:${target_name}>"
    COMMENT "Copying prebuilt runtime DLLs for ${target_name}")
endfunction()

# * 在指定包的已选配置目录中查库，ARGN 必须由独立 Find 模块给出候选库名。
# * out_var 返回链接文件；Windows shared 还返回 out_var_RUNTIME 并登记部署路径。
# * MSVC 的 Debug/RelWithDebInfo 若发现符号，额外返回 out_var_PDBS。
# * 附加输出仅在对应分支成功赋值，调用者复用变量名时须自行处理旧值。
# * 查找不使用系统默认目录，也不回退到源码；库缺失与必需 DLL 缺失均为致命错误。
# * 返回路径证明文件可定位，不检验机器架构、导出符号、运行库或包间 ABI 兼容性。
# * Find 模块必须提供完整且有序的候选名，列表优先级也会影响运行时候选构造。
# * 本函数只导出路径，导入目标的配置属性、头路径和系统链接闭包归具体模块维护。
function(ice_prebuilt_find_library out_var package config)
  # 具体名称由包自己维护，helper 不根据包名自动猜测库或创建通用导入目标。
  if(ARGN)
    # 保留调用者提供的命名兼容顺序，不重排 lib 前缀或调试后缀的优先级。
    set(_prebuilt_library_names ${ARGN})
  else()
    message(FATAL_ERROR "ice_prebuilt_find_library 缺少库候选名：${package}")
  endif()

  ice_prebuilt_library_dir(_library_dir "${package}" "${config}")
  # 先选目录再查文件；较优先目录存在但缺库时直接失败，不尝试下一个候选目录。
  find_library(
    _prebuilt_library
    NAMES ${_prebuilt_library_names}
    PATHS "${_library_dir}"
    # NO_CACHE 避免本 helper 的通用结果变量污染之后其他包或配置的查询。
    NO_DEFAULT_PATH NO_CACHE)
  if(NOT _prebuilt_library)
    # 禁止从系统库路径补齐，报错同时列出目录与名称以定位打包缺项。
    message(
      FATAL_ERROR
        "缺少 ${package} 的 ${config} 引擎预编译库。搜索目录：${_library_dir}；候选名：${_prebuilt_library_names}"
    )
  endif()
  set(${out_var}
      "${_prebuilt_library}"
      PARENT_SCOPE)

  if(ICE_LINKAGE STREQUAL "shared" AND WIN32)
    # Windows 导入库本身不能运行，配置期同步要求对应的运行时目录和 DLL。
    ice_prebuilt_runtime_dir(_runtime_dir "${package}" "${config}")
    set(_runtime_names "")
    # 每次查询重新建立 DLL 候选，避免上一库的名称混入当前包查找。
    foreach(_library_name IN LISTS _prebuilt_library_names)
      # 保持库名候选的先后顺序，只追加平台共享后缀，不自动去掉 lib 前缀。
      list(APPEND _runtime_names
           "${_library_name}${CMAKE_SHARED_LIBRARY_SUFFIX}")
    endforeach()
    # 精确名字优先于版本化 glob，避免已有明确匹配时误挑另一版 DLL。
    find_file(
      _prebuilt_runtime
      NAMES ${_runtime_names}
      PATHS "${_runtime_dir}"
      NO_DEFAULT_PATH NO_CACHE)
    if(NOT _prebuilt_runtime)
      # 兼容诸如 avcodec-<版本>.dll 的打包形式；匹配集合仍限定在本包配置目录。
      set(_runtime_glob_matches "")
      foreach(_runtime_name IN LISTS _runtime_names)
        # 先去掉扩展名再插入版本通配符，匹配的仍是同一候选名字族。
        get_filename_component(_runtime_stem "${_runtime_name}" NAME_WE)
        # 不递归扫描子目录，也不遍历 DLL 的导入表补齐间接依赖。
        file(GLOB _runtime_versioned_matches CONFIGURE_DEPENDS
             "${_runtime_dir}/${_runtime_stem}-*${CMAKE_SHARED_LIBRARY_SUFFIX}")
        list(APPEND _runtime_glob_matches ${_runtime_versioned_matches})
      endforeach()
      if(_runtime_glob_matches)
        # 字典序首项不是语义版本最大值，多版本共存时需由打包方消除歧义。
        list(SORT _runtime_glob_matches)
        list(GET _runtime_glob_matches 0 _prebuilt_runtime)
        # 只登记选中的一个匹配，不将同目录所有版本 DLL 全部复制到目标旁。
      endif()
    endif()
    if(NOT _prebuilt_runtime)
      # 已找到导入库仍不足以成功，缺 DLL 不能推迟到运行时才暴露。
      message(
        FATAL_ERROR
          "缺少 ${package} 的 ${config} 引擎动态运行时文件。搜索目录：${_runtime_dir}；候选名：${_runtime_names}"
      )
    endif()
    set(${out_var}_RUNTIME
        "${_prebuilt_runtime}"
        PARENT_SCOPE)
    # 登记具体路径供后续部署，不在这里复制，以免目标输出目录尚未确定。
    ice_prebuilt_record_runtime_file("${_prebuilt_runtime}")
  endif()

  if(MSVC)
    # PDB 分支看的是请求配置；Release 即使回退到 RelWithDebInfo 目录也不进入。
    string(TOLOWER "${config}" _prebuilt_config_lower)
    if(_prebuilt_config_lower STREQUAL "debug" OR _prebuilt_config_lower
                                                  STREQUAL "relwithdebinfo")
      # 编译器内嵌符号不由此分支导出，只有 MSVC 的旁路 PDB 进入附加输出。
      ice_prebuilt_find_symbol_files(_prebuilt_symbol_files "${package}"
                                     "${config}")
      # 缺符号不报错且不清旧输出，此行为不能代替发布包符号完整性检查。
      if(_prebuilt_symbol_files)
        set(${out_var}_PDBS
            ${_prebuilt_symbol_files}
            PARENT_SCOPE)
      endif()
    endif()
  endif()
endfunction()
