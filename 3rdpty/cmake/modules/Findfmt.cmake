# 导入 fmt 预编译库，导出源码构建同名目标：fmt::fmt。
include("${CMAKE_CURRENT_LIST_DIR}/PrebuiltLayout.cmake")
# 默认查找引擎自己的包根，显式 ICE_PREBUILT_ROOT 可覆盖此位置。
ice_prebuilt_init("${CMAKE_CURRENT_LIST_DIR}/../../prebuilts")
# 公共头文件不按配置复制，版本须与各平台编译库相匹配。
ice_prebuilt_include_dir(_fmt_include_dir fmt)

# 保留已存在目标，避免重入时改写其他依赖目录提供的 fmt。
if(NOT TARGET fmt::fmt)
  # GLOBAL 使命名空间导入目标可被平级目录共享，不生成新二进制。 本入口导入编译库，不创建 header-only 替代形式。
  add_library(fmt::fmt UNKNOWN IMPORTED GLOBAL)
  # include 作为使用要求传播，消费者无需猜测预编译包根路径。
  set_target_properties(fmt::fmt PROPERTIES INTERFACE_INCLUDE_DIRECTORIES
                                            "${_fmt_include_dir}")

  # 生成器决定所需配置集合；每种配置的库路径分别解析。
  ice_prebuilt_target_configs(_fmt_configs)
  set(_fmt_imported_configs "")
  # 默认位置从当前模块首个结果确定，不继承其他包的查找状态。
  set(_fmt_default_library "")
  foreach(_fmt_config IN LISTS _fmt_configs)
    # 每个配置查找都可能失败；列表顺序只决定无配置时的通用位置，不改变请求本身。 大写用于导入属性，磁盘目录名由布局 helper 独立映射。
    string(TOUPPER "${_fmt_config}" _fmt_config_upper)
    # 名称候选兼容调试后缀及平台 lib 前缀，但只在指定配置目录查找。
    ice_prebuilt_find_library(
      _fmt_library
      fmt
      "${_fmt_config}"
      fmt
      # 是否带 d 不决定运行库配置，Debug 与发布隔离仍依赖目录。
      fmtd
      libfmt
      # Windows shared 还须提供对应 DLL，helper 会登记其部署路径。
      libfmtd)
    # 发布型请求可映射到含符号包，导入属性仍使用请求配置名称。
    list(APPEND _fmt_imported_configs "${_fmt_config_upper}")
    set_target_properties(
      fmt::fmt PROPERTIES "IMPORTED_LOCATION_${_fmt_config_upper}"
                          "${_fmt_library}")
    # 首个结果只作通用回退，后续配置仍保留各自位置。
    if(_fmt_default_library STREQUAL "")
      set(_fmt_default_library "${_fmt_library}")
    endif()
  endforeach()
  # 单配置未选构建类型时补 NOCONFIG，不要求额外磁盘目录。
  if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set_target_properties(fmt::fmt PROPERTIES IMPORTED_LOCATION_NOCONFIG
                                              "${_fmt_default_library}")
  endif()
  set_target_properties(
    fmt::fmt PROPERTIES IMPORTED_CONFIGURATIONS "${_fmt_imported_configs}"
                        IMPORTED_LOCATION "${_fmt_default_library}")
endif()

# 目录或库缺失已由 helper 中止配置，这里不把缺包变成软成功。
set(fmt_FOUND TRUE)
