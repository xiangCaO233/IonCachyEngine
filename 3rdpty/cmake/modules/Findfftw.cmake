# 导入 FFTW 预编译库，并提供引擎包装目标：3rd_fftw3。
include("${CMAKE_CURRENT_LIST_DIR}/PrebuiltLayout.cmake")
# 只查引擎打包的 FFTW，禁止悄悄混入系统安装版本。
ice_prebuilt_init("${CMAKE_CURRENT_LIST_DIR}/../../prebuilts")
# 公共头文件根不带配置后缀，库配置在下方分别绑定。
ice_prebuilt_include_dir(_fftw_include_dir fftw)

# 同名底层目标已存在时保留其来源与依赖属性。
if(NOT TARGET fftw3)
  # 这里导入双精度 fftw3，不额外引入 fftw3f 或线程变体。
  add_library(fftw3 UNKNOWN IMPORTED GLOBAL)
  set_target_properties(fftw3 PROPERTIES INTERFACE_INCLUDE_DIRECTORIES
                                         "${_fftw_include_dir}")

  # 按生成器请求的每个配置检查包，不能只验证单一默认库。
  ice_prebuilt_target_configs(_fftw_configs)
  set(_fftw_imported_configs "")
  # 清空本模块通用位置，避免重入后保留另一配置结果。
  set(_fftw_default_library "")
  foreach(_fftw_config IN LISTS _fftw_configs)
    # 大写用于 CMake 属性，磁盘名称由布局层处理大小写候选。
    string(TOUPPER "${_fftw_config}" _fftw_config_upper)
    ice_prebuilt_find_library(_fftw_library fftw "${_fftw_config}" fftw3
                              libfftw3)
    # 在当前目标上登记请求配置，发布型配置允许选带符号目录。
    list(APPEND _fftw_imported_configs "${_fftw_config_upper}")
    set_target_properties(
      fftw3 PROPERTIES "IMPORTED_LOCATION_${_fftw_config_upper}"
                       "${_fftw_library}")
    # 首个结果只提供通用回退，不代替专属配置位置。
    if(_fftw_default_library STREQUAL "")
      set(_fftw_default_library "${_fftw_library}")
    endif()
  endforeach()
  # 无配置生成器需 NOCONFIG，否则只设置发布属性可能无法解析位置。
  if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set_target_properties(fftw3 PROPERTIES IMPORTED_LOCATION_NOCONFIG
                                           "${_fftw_default_library}")
  endif()
  set_target_properties(
    fftw3 PROPERTIES IMPORTED_CONFIGURATIONS "${_fftw_imported_configs}"
                     IMPORTED_LOCATION "${_fftw_default_library}")
endif()

# 包装名与引擎源码模式一致，底层 fftw3 也保留给直接使用者。
if(NOT TARGET 3rd_fftw3)
  # INTERFACE 不产出新归档，避免把同一 FFTW 二进制重复打包。
  add_library(3rd_fftw3 INTERFACE)
  # 底层头目录和配置库一并传递，Rubber Band 不需手动重建其路径。
  target_link_libraries(3rd_fftw3 INTERFACE fftw3)
endif()

# 包缺失已由布局 helper 中止，FOUND 表示完整解析或有效目标复用完成。
set(fftw_FOUND TRUE)
