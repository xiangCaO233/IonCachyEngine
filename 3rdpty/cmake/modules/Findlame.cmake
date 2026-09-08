# 导入 LAME 预编译库，导出源码构建同名目标：3rd_lame。
include("${CMAKE_CURRENT_LIST_DIR}/PrebuiltLayout.cmake")
# 仅解析引擎 LAME 包；FFmpeg 静态链接从这个目标取得编码实现。
ice_prebuilt_init("${CMAKE_CURRENT_LIST_DIR}/../../prebuilts")
# 头文件位于公共根，与 Debug/发布二进制目录分离。
ice_prebuilt_include_dir(_lame_include_dir lame)

# 复用已有目标避免重复声明，消费名称与源码模式保持相同。
if(NOT TARGET 3rd_lame)
  # 本文件只描述预编译库，不创建外部编码器编译任务。 GLOBAL 使 FFmpeg 聚合目标可从平级目录引用相同导入对象。
  add_library(3rd_lame UNKNOWN IMPORTED GLOBAL)
  set_target_properties(3rd_lame PROPERTIES INTERFACE_INCLUDE_DIRECTORIES
                                            "${_lame_include_dir}")

  # 为每个请求配置独立选库，不能只导入首个发布文件。
  ice_prebuilt_target_configs(_lame_configs)
  # shared/Windows 查找会额外验证运行时 DLL，不仅验证链接时的导入库。
  set(_lame_imported_configs "")
  # 默认位置只来自本次 LAME 查找，防止跨包局部变量残留。
  set(_lame_default_library "")
  foreach(_lame_config IN LISTS _lame_configs)
    # 每个配置查找都可能失败；列表顺序只决定无配置时的通用位置，不改变请求本身。 属性名称用大写配置，真实目录按布局 helper 的候选顺序选择。
    string(TOUPPER "${_lame_config}" _lame_config_upper)
    ice_prebuilt_find_library(_lame_library lame "${_lame_config}" mp3lame
                              libmp3lame libmp3lame-static)
    # 不同 mp3lame 文件名只是平台打包差异，不改变编码目标或查找范围。
    list(APPEND _lame_imported_configs "${_lame_config_upper}")
    set_target_properties(
      3rd_lame PROPERTIES "IMPORTED_LOCATION_${_lame_config_upper}"
                          "${_lame_library}")
    # 首个结果用于通用位置，逐配置映射仍各自保留。
    if(_lame_default_library STREQUAL "")
      set(_lame_default_library "${_lame_library}")
    endif()
  endforeach()
  # 无构建类型时补 NOCONFIG，默认包选择遵循发布配置回退规则。
  if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set_target_properties(3rd_lame PROPERTIES IMPORTED_LOCATION_NOCONFIG
                                              "${_lame_default_library}")
  endif()
  # 显式列出可用配置，避免依赖 CMake 对未登记导入配置的隐式选择。
  set_target_properties(
    3rd_lame PROPERTIES IMPORTED_CONFIGURATIONS "${_lame_imported_configs}"
                        IMPORTED_LOCATION "${_lame_default_library}")
endif()

# 缺失库或头目录已终止配置，不能把缺 LAME 当作可选编码功能。
set(lame_FOUND TRUE)
