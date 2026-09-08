# 导入 zlib 预编译库，导出源码构建同名目标：3rd_zlib。
include("${CMAKE_CURRENT_LIST_DIR}/PrebuiltLayout.cmake")
# 引擎包根与系统 zlib 分离，布局缺失必须在配置期失败。
ice_prebuilt_init("${CMAKE_CURRENT_LIST_DIR}/../../prebuilts")
# 头文件采用跨配置共享路径，不能从 Debug 库目录推导 include。
ice_prebuilt_include_dir(_zlib_include_dir zlib)

# 目标名与源码模式保持一致，已有目标由原提供方维护。
if(NOT TARGET 3rd_zlib)
  # 导入现有压缩库，不触发 zlib 自身的构建或下载。 GLOBAL 保证平级 FFmpeg 查找模块也能复用同一个压缩依赖对象。
  add_library(3rd_zlib UNKNOWN IMPORTED GLOBAL)
  # 压缩接口头路径随目标传播，FFmpeg 不需要重复定位 zlib 的公共头。
  set_target_properties(3rd_zlib PROPERTIES INTERFACE_INCLUDE_DIRECTORIES
                                            "${_zlib_include_dir}")

  # 分别解析生成器请求的配置，避免调试与发布运行库混用。
  ice_prebuilt_target_configs(_zlib_configs)
  set(_zlib_imported_configs "")
  # 通用位置仅从本包查找结果选择，不复用上层遗留变量。
  set(_zlib_default_library "")
  foreach(_zlib_config IN LISTS _zlib_configs)
    # 每个配置查找都可能失败；列表顺序只决定无配置时的通用位置，不改变请求本身。 属性后缀统一大写，实际目录保留布局约定。
    string(TOUPPER "${_zlib_config}" _zlib_config_upper)
    # z、zlib 及静态后缀覆盖不同工具链产物名，目录仍限定在当前包。
    ice_prebuilt_find_library(
      _zlib_library
      # 这些候选描述同一压缩 API，不能用不同架构的同名文件替代。
      zlib
      "${_zlib_config}"
      z
      # lib 前缀是打包名称兼容候选，不代表另一种依赖来源。
      libz
      zlib
      libzs
      # shared/Windows 的运行时 DLL 由 helper 同步验证与登记。
      libzsd)
    # 配置属性使用请求名，发布请求优先复用带符号发布目录。
    list(APPEND _zlib_imported_configs "${_zlib_config_upper}")
    set_target_properties(
      3rd_zlib PROPERTIES "IMPORTED_LOCATION_${_zlib_config_upper}"
                          "${_zlib_library}")
    # 首个有效库仅提供无配置回退，各配置仍有独立绑定。
    if(_zlib_default_library STREQUAL "")
      set(_zlib_default_library "${_zlib_library}")
    endif()
  endforeach()
  # 没有构建类型时需要 NOCONFIG，不能只登记 Release 属性。
  if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set_target_properties(3rd_zlib PROPERTIES IMPORTED_LOCATION_NOCONFIG
                                              "${_zlib_default_library}")
  endif()
  set_target_properties(
    3rd_zlib PROPERTIES IMPORTED_CONFIGURATIONS "${_zlib_imported_configs}"
                        IMPORTED_LOCATION "${_zlib_default_library}")
endif()

# 库和头文件目录缺失已由 helper 报错，只有成功导入才到达此处。
set(zlib_FOUND TRUE)
