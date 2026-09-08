# 导入 libsamplerate 预编译库，并提供引擎包装目标：3rd_libsamplerate。
include("${CMAKE_CURRENT_LIST_DIR}/PrebuiltLayout.cmake")
# 路径局限于引擎自己的包树，不自动使用宿主或系统采样率库。
ice_prebuilt_init("${CMAKE_CURRENT_LIST_DIR}/../../prebuilts")
# 头目录共享于各配置，版本兼容由预编译包维护者保证。
ice_prebuilt_include_dir(_samplerate_include_dir libsamplerate)

# 底层目标已由其他模块提供时保留原定义，不改写其链接配置。
if(NOT TARGET samplerate)
  # 导入已有采样率转换实现，不在本文件创建源码构建任务。
  add_library(samplerate UNKNOWN IMPORTED GLOBAL)
  set_target_properties(samplerate PROPERTIES INTERFACE_INCLUDE_DIRECTORIES
                                              "${_samplerate_include_dir}")

  # Debug 与发布按不同配置查找，避免跨运行库混用。
  ice_prebuilt_target_configs(_samplerate_configs)
  # Windows shared 的 DLL 检查由查库 helper 承担，本包装不重复部署文件。
  set(_samplerate_imported_configs "")
  # 通用位置从本模块首个结果取得，不沿用其他 Find 文件的变量。
  set(_samplerate_default_library "")
  foreach(_samplerate_config IN LISTS _samplerate_configs)
    # 导入属性使用请求配置名；库目录可按发布型规则回退。
    string(TOUPPER "${_samplerate_config}" _samplerate_config_upper)
    ice_prebuilt_find_library(_samplerate_library libsamplerate
                              "${_samplerate_config}" samplerate libsamplerate)
    # 找到库才登记配置，缺目录或缺文件在 helper 中立即失败。
    list(APPEND _samplerate_imported_configs "${_samplerate_config_upper}")
    set_target_properties(
      samplerate PROPERTIES "IMPORTED_LOCATION_${_samplerate_config_upper}"
                            "${_samplerate_library}")
    # 首个通用位置保持稳定，各显式请求仍有单独的导入路径。
    if(_samplerate_default_library STREQUAL "")
      set(_samplerate_default_library "${_samplerate_library}")
    endif()
  endforeach()
  # 单配置未选类型时补 NOCONFIG，磁盘默认包仍按布局约定选择。
  if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set_target_properties(
      samplerate PROPERTIES IMPORTED_LOCATION_NOCONFIG
                            "${_samplerate_default_library}")
  endif()
  set_target_properties(
    samplerate
    PROPERTIES IMPORTED_CONFIGURATIONS "${_samplerate_imported_configs}"
               IMPORTED_LOCATION "${_samplerate_default_library}")
endif()

# 包装入口保持源码模式命名，同时允许直接消费底层 samplerate 目标。
if(NOT TARGET 3rd_libsamplerate)
  # 轻量包装不生成额外库文件，也不重复包含转换实现。
  add_library(3rd_libsamplerate INTERFACE)
  # 转发公共包含目录与逐配置链接文件，消费者不应自行拼接预编译路径。
  target_link_libraries(3rd_libsamplerate INTERFACE samplerate)
endif()

# 本库及头目录已检查完毕，FOUND 不会隐式启用源码回退。
set(libsamplerate_FOUND TRUE)
