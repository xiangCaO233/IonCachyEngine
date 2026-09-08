# 导入 OpenAL 预编译库，导出源码构建同名目标：OpenAL::OpenAL。
include("${CMAKE_CURRENT_LIST_DIR}/PrebuiltLayout.cmake")
# 默认包根属于引擎自身，可由 ICE_PREBUILT_ROOT 覆盖；不搜索宿主的另一个包树。
ice_prebuilt_init("${CMAKE_CURRENT_LIST_DIR}/../../prebuilts")
# 包目录存在仅是基础校验，具体头文件与二进制匹配仍需编译确认。
ice_prebuilt_include_dir(_openal_include_dir openal)
set(_openal_include_dirs "${_openal_include_dir}" "${_openal_include_dir}/AL")
# 同时暴露根和 AL 子目录，兼容 <AL/al.h> 与后端现有 <al.h> 两种包含方式。

if(NOT TARGET OpenAL::OpenAL)
  # 目标名与源码模式一致；GLOBAL 使平级目录也能消费同一导入目标。 UNKNOWN 保留已有导入约定，具体静态库或导入库由布局和链接偏好选择。
  add_library(OpenAL::OpenAL UNKNOWN IMPORTED GLOBAL)
  set_target_properties(OpenAL::OpenAL PROPERTIES INTERFACE_INCLUDE_DIRECTORIES
                                                  "${_openal_include_dirs}")

  ice_prebuilt_target_configs(_openal_configs)
  # 每个请求配置独立寻找二进制，不能把 Debug 请求绑定到发布运行库。
  set(_openal_imported_configs "")
  # 清空模块局部结果，避免重复查找保留上一配置的默认位置。
  set(_openal_default_library "")
  foreach(_openal_config IN LISTS _openal_configs)
    # 属性后缀使用大写，磁盘配置目录则由布局 helper 维护大小写候选。
    string(TOUPPER "${_openal_config}" _openal_config_upper)
    # 候选名覆盖不同平台包名，但查找被限制在已选平台、架构和工具链目录。 shared/Windows 还会验证相应 DLL
    # 并登记部署清单，不只检查导入库存在。
    ice_prebuilt_find_library(
      _openal_library
      openal
      "${_openal_config}"
      OpenAL
      # 候选名仅在当前包目录匹配，不跨工具链复用同名 OpenAL 文件。
      OpenAL32
      openal
      # 文件名顺序保持传统 OpenAL 与 OpenAL Soft 包兼容，不在运行时切换驱动。
      soft_oal)
    list(APPEND _openal_imported_configs "${_openal_config_upper}")
    # 记录的是请求配置名；实际 Release 也可能按包约定使用 RelWithDebInfo。
    set_target_properties(
      OpenAL::OpenAL PROPERTIES "IMPORTED_LOCATION_${_openal_config_upper}"
                                "${_openal_library}")
    if(_openal_default_library STREQUAL "")
      # 首个解析结果仅作为无配置位置，不覆盖各配置专属的导入位置。
      set(_openal_default_library "${_openal_library}")
    endif()
  endforeach()
  if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    # 单配置生成器未选配置时补 NOCONFIG，避免无配置消费者无库可链接。
    set_target_properties(
      OpenAL::OpenAL PROPERTIES IMPORTED_LOCATION_NOCONFIG
                                "${_openal_default_library}")
  endif()
  set_target_properties(
    OpenAL::OpenAL
    # 同时列出可用配置与通用位置，供 CMake 解析导入目标的配置映射。
    PROPERTIES IMPORTED_CONFIGURATIONS "${_openal_imported_configs}"
               IMPORTED_LOCATION "${_openal_default_library}")

  if(ICE_LINKAGE STREQUAL "static")
    # OpenAL 头文件默认按 DLL 导入声明符号；静态预编译库必须显式关闭 dllimport，否则 MinGW 会查找 __imp_al*
    # 导入符号。
    target_compile_definitions(
      OpenAL::OpenAL INTERFACE AL_LIBTYPE_STATIC ICE_OPENALSOFT_STATIC_LINKAGE)
    # 第二个宏仅允许静态 OpenAL Soft 使用额外日志钩子，动态包不承诺该 ABI。
  endif()

  if(WIN32)
    # Windows 后端需要传统音频、多媒体调度和 COM 系统入口。
    target_link_libraries(OpenAL::OpenAL INTERFACE winmm ole32 avrt)
  elseif(APPLE)
    # Apple 平台依赖 framework，由最终消费者继承，不硬编码 SDK 绝对路径。
    target_link_libraries(
      OpenAL::OpenAL INTERFACE "-framework CoreAudio" "-framework AudioToolbox"
                               "-framework AudioUnit")
  else()
    # Unix 后端动态加载所需入口仍是系统依赖，与引擎依赖来源开关无关。
    target_link_libraries(OpenAL::OpenAL INTERFACE dl)
  endif()
endif()

# 布局或库缺失时 helper 已终止配置，只有完成查找或复用已有目标才设置 FOUND。 已有目标时其系统库与静态宏由原提供方维护，本模块不覆盖。
set(OpenAL_FOUND TRUE)
