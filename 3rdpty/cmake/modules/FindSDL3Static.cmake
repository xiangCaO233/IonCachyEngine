# 导入 SDL3 预编译库，并提供引擎包装目标：3rd_sdl3。
include("${CMAKE_CURRENT_LIST_DIR}/PrebuiltLayout.cmake")
# 只解析 ICE 自己的预编译布局，缺包直接报错，不隐式编译 SDL 源码。
ice_prebuilt_init("${CMAKE_CURRENT_LIST_DIR}/../../prebuilts")
# 目录存在不验证每个 SDL3 头的版本，包 ABI 仍须由打包流程保持一致。
ice_prebuilt_include_dir(_sdl_include_dir sdl)

if(NOT TARGET SDL3-static)
  # 兼容既有目标名，即使 shared 偏好也保留 SDL3-static 名称作为稳定消费入口。
  # 名字不决定链接类型，二进制目录与候选名顺序才负责选择静态库或导入库。 GLOBAL 使包装目标和其他平级目录可以引用同一导入对象。
  add_library(SDL3-static UNKNOWN IMPORTED GLOBAL)
  set_target_properties(SDL3-static PROPERTIES INTERFACE_INCLUDE_DIRECTORIES
                                               "${_sdl_include_dir}")

  ice_prebuilt_target_configs(_sdl_configs)
  # 多配置生成器需准备每一种请求配置，单配置只解析当前构建所需项。
  set(_sdl_imported_configs "")
  # 查找结果每轮从空值建立，不能继承另一配置集合的通用路径。
  set(_sdl_default_library "")
  foreach(_sdl_config IN LISTS _sdl_configs)
    # 每个配置查找都可能失败；列表顺序只决定无配置时的通用位置，不改变请求本身。 每项解析结束后只修改导入属性，不启动 SDL 初始化或探测实际声卡。
    # CMake 导入属性以大写配置名区分，目录映射由布局 helper 独立完成。
    string(TOUPPER "${_sdl_config}" _sdl_config_upper)
    if(ICE_LINKAGE STREQUAL "shared")
      # 动态包优先无 static 后缀名字，保留旧打包命名作为末位兼容候选。
      set(_sdl_library_names SDL3 libSDL3 SDL3-static)
    else()
      # 静态包优先明确的 static 名字，但 MinGW 常规 libSDL3 也可能是静态归档。
      set(_sdl_library_names SDL3-static SDL3 libSDL3)
    endif()
    # 仅在选定库目录内查找；shared/Windows 同时要求 DLL 运行时存在。
    ice_prebuilt_find_library(_sdl_library sdl "${_sdl_config}"
                              ${_sdl_library_names})
    # 仅成功查找到的配置才登记，缺文件由 helper 报错而非跳过。
    list(APPEND _sdl_imported_configs "${_sdl_config_upper}")
    # 使用请求配置作为属性键，发布型配置可复用含符号的 RelWithDebInfo 包。
    set_target_properties(
      SDL3-static PROPERTIES "IMPORTED_LOCATION_${_sdl_config_upper}"
                             "${_sdl_library}")
    if(_sdl_default_library STREQUAL "")
      # 默认位置使用首个成功结果，各显式配置仍使用各自的库路径。
      set(_sdl_default_library "${_sdl_library}")
    endif()
  endforeach()
  if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    # 无构建类型的单配置入口仍需要 NOCONFIG 导入位置。
    set_target_properties(SDL3-static PROPERTIES IMPORTED_LOCATION_NOCONFIG
                                                 "${_sdl_default_library}")
  endif()
  set_target_properties(
    SDL3-static PROPERTIES IMPORTED_CONFIGURATIONS "${_sdl_imported_configs}"
                           IMPORTED_LOCATION "${_sdl_default_library}")
endif()

if(NOT TARGET 3rd_sdl3)
  # 接口包装集中传播 SDL 及其系统链接闭包，不产生新的二进制产物。 包装与底层导入目标分别去重，允许其他目录已经创建底层目标的情况。
  add_library(3rd_sdl3 INTERFACE)
  # SDL 的公共包含目录随底层目标传播，不要求业务层再手动补头路径。
  target_link_libraries(3rd_sdl3 INTERFACE SDL3-static)
  if(WIN32 AND NOT ICE_LINKAGE STREQUAL "shared")
    # 动态 SDL 把实现依赖留在 DLL 内；静态归档需由最终可执行文件解析这些入口。 SDL3 静态库不会携带 Windows
    # 系统库信息，导入目标必须补齐源码目标的链接闭包。
    target_link_libraries(
      3rd_sdl3
      INTERFACE kernel32
                # 窗口、图形和输入相关入口来自 SDL 构建时启用的 Windows 后端。
                user32
                gdi32
                winmm
                imm32
                # COM、版本和设备枚举接口与音频设备管理共同构成静态链接闭包。
                ole32
                oleaut32
                # 版本资源、GUID 与设备安装接口属于预编译 SDL 启用的平台功能。
                version
                uuid
                advapi32
                setupapi
                shell32
                # DirectInput 在该静态包的链接闭包中，即使业务仅使用音频也保留此依赖。
                dinput8)
  endif()
endif()

# FOUND 名称保持与 find_package(SDL3Static) 一致，与实际链接偏好无关。
set(SDL3Static_FOUND TRUE)
