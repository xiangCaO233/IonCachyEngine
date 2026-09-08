# 导入 Rubber Band 预编译库，导出源码构建同名目标：3rd_rubberband。
include("${CMAKE_CURRENT_LIST_DIR}/PrebuiltLayout.cmake")
# Rubber Band 的路径按引擎独立布局解析，不读取宿主包根。
ice_prebuilt_init("${CMAKE_CURRENT_LIST_DIR}/../../prebuilts")
# 预编译配置使用 FFTW 变换后端，其二进制必须先建立稳定目标。
find_package(fftw REQUIRED)
# 重采样依赖独立导入，缺包不能仅凭主归档存在而继续配置。
find_package(libsamplerate REQUIRED)
# 所有构建配置共享头文件版本，ABI 匹配由打包流程保证。
ice_prebuilt_include_dir(_rubberband_include_dir rubberband)

# 保留源码模式使用的目标名，不让业务模块区分依赖来源。
if(NOT TARGET 3rd_rubberband)
  # 导入目标不执行上游构建，仅描述已有库和消费要求。
  add_library(3rd_rubberband UNKNOWN IMPORTED GLOBAL)
  set_target_properties(3rd_rubberband PROPERTIES INTERFACE_INCLUDE_DIRECTORIES
                                                  "${_rubberband_include_dir}")

  # 每种生成器配置独立查找，不能将 Debug 映射到发布 CRT。
  ice_prebuilt_target_configs(_rubberband_configs)
  # 配置循环不运行变速算法，预编译库的实时行为由独立音频测试验证。
  set(_rubberband_imported_configs "")
  # 首个结果用于通用位置，所有显式配置仍单独保留。
  set(_rubberband_default_library "")
  foreach(_rubberband_config IN LISTS _rubberband_configs)
    # 请求名是属性键，磁盘发布目录由 helper 优先选 RelWithDebInfo。
    string(TOUPPER "${_rubberband_config}" _rubberband_config_upper)
    # 静态后缀和 lib 前缀仅是同一选定目录下的文件名候选。
    ice_prebuilt_find_library(
      _rubberband_library
      rubberband
      "${_rubberband_config}"
      rubberband
      # 名称本身不决定链接偏好，static/shared 目录已经在布局层分离。
      rubberband-static
      librubberband
      # Windows shared 还会验证运行时文件，并汇入部署清单。
      librubberband-static)
    # 将请求配置登记到当前目标，不覆盖其他依赖的导入配置。
    list(APPEND _rubberband_imported_configs "${_rubberband_config_upper}")
    set_target_properties(
      3rd_rubberband PROPERTIES "IMPORTED_LOCATION_${_rubberband_config_upper}"
                                "${_rubberband_library}")
    # 首次选择后保持通用位置稳定，后续循环只增加专属配置。
    if(_rubberband_default_library STREQUAL "")
      set(_rubberband_default_library "${_rubberband_library}")
    endif()
  endforeach()
  # 无配置消费者需要 NOCONFIG 位置，默认查找仍遵循发布包约定。
  if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set_target_properties(
      3rd_rubberband PROPERTIES IMPORTED_LOCATION_NOCONFIG
                                "${_rubberband_default_library}")
  endif()
  set_target_properties(
    3rd_rubberband
    PROPERTIES IMPORTED_CONFIGURATIONS "${_rubberband_imported_configs}"
               IMPORTED_LOCATION "${_rubberband_default_library}")
  # 将实际变换与重采样实现传播到最终链接目标，归档本身不包含这些库。
  target_link_libraries(3rd_rubberband INTERFACE 3rd_fftw3 3rd_libsamplerate)
  # 平台线程库与数学库分别补入，避免在 MSVC 上请求 MinGW pthread。
  if(WIN32)
    # MinGW 包的 pthread 实现由对应工具链提供，不手写其他运行库路径。
    if(NOT MSVC)
      target_link_libraries(3rd_rubberband INTERFACE pthread)
    endif()
  else()
    # Unix 数学符号由系统库解析，不是额外的源码依赖构建。
    target_link_libraries(3rd_rubberband INTERFACE m)
    # Apple 平台不在此添加独立 pthread 项，沿用平台运行时约定。
    if(NOT APPLE)
      target_link_libraries(3rd_rubberband INTERFACE pthread)
    endif()
  endif()
endif()

# 依赖目标和本库全部解析后才报告成功，缺任一包均已提前报错。
set(rubberband_FOUND TRUE)
