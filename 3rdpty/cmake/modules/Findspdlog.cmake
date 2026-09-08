# 导入 spdlog 预编译库，导出源码构建同名目标：spdlog::spdlog。
include("${CMAKE_CURRENT_LIST_DIR}/PrebuiltLayout.cmake")
# 路径只来自引擎包布局，不自动使用系统安装的另一个日志库。
ice_prebuilt_init("${CMAKE_CURRENT_LIST_DIR}/../../prebuilts")
# 包使用外部 fmt，不能用 spdlog 内置的 fmt 类型替代链接 ABI。
find_package(fmt REQUIRED)
# 共享头文件版本需与编译库一致，目录存在只完成基础校验。
ice_prebuilt_include_dir(_spdlog_include_dir spdlog)

# 已有目标的宏和依赖由提供方维护，本模块不二次改写。
if(NOT TARGET spdlog::spdlog)
  # 导入现有库并全局暴露，业务目录无需分别添加裸库路径。
  add_library(spdlog::spdlog UNKNOWN IMPORTED GLOBAL)
  set_target_properties(spdlog::spdlog PROPERTIES INTERFACE_INCLUDE_DIRECTORIES
                                                  "${_spdlog_include_dir}")

  # 配置集合逐项解析，Debug 不与发布日志库混用。
  ice_prebuilt_target_configs(_spdlog_configs)
  # 配置列表仅属于 spdlog，不复用 fmt 模块的局部结果。
  set(_spdlog_imported_configs "")
  # 通用位置从本轮首个查找结果取得，避免残留旧配置路径。
  set(_spdlog_default_library "")
  foreach(_spdlog_config IN LISTS _spdlog_configs)
    # 请求名转大写作为属性键，库目录回退规则留给布局 helper。
    string(TOUPPER "${_spdlog_config}" _spdlog_config_upper)
    # 在已选择的编译器与配置目录内匹配不同平台库名，不扩大系统搜索。
    ice_prebuilt_find_library(
      _spdlog_library
      spdlog
      "${_spdlog_config}"
      spdlog
      # 调试后缀只兼容打包名称，不代替配置目录对 CRT 的隔离。
      spdlogd
      libspdlog
      # shared/Windows 同时查验 DLL，导入库存在不等于运行时齐全。
      libspdlogd)
    # 属性记录请求配置，即使 Release 实际选用 RelWithDebInfo 包。
    list(APPEND _spdlog_imported_configs "${_spdlog_config_upper}")
    set_target_properties(
      spdlog::spdlog PROPERTIES "IMPORTED_LOCATION_${_spdlog_config_upper}"
                                "${_spdlog_library}")
    # 首个通用位置不覆盖逐配置导入属性。
    if(_spdlog_default_library STREQUAL "")
      set(_spdlog_default_library "${_spdlog_library}")
    endif()
  endforeach()
  # 未选择构建类型时补 NOCONFIG，供无配置消费路径使用。
  if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set_target_properties(
      spdlog::spdlog PROPERTIES IMPORTED_LOCATION_NOCONFIG
                                "${_spdlog_default_library}")
  endif()
  set_target_properties(
    spdlog::spdlog
    PROPERTIES IMPORTED_CONFIGURATIONS "${_spdlog_imported_configs}"
               IMPORTED_LOCATION "${_spdlog_default_library}")
  # 预编译包链接的是 spdlog 编译库，消费者必须关闭 header-only 实现。
  target_compile_definitions(spdlog::spdlog INTERFACE SPDLOG_COMPILED_LIB
                                                      SPDLOG_FMT_EXTERNAL)
  # 外部 fmt 的头和链接依赖一起传播，保持消费者与预编译库的格式化实现一致。
  target_link_libraries(spdlog::spdlog INTERFACE fmt::fmt)
endif()

# 全部配置已解析或复用现有目标后才报告找到包，缺失路径不会静默回退。
set(spdlog_FOUND TRUE)
