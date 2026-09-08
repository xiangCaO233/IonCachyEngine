# 导入 FFmpeg 预编译组件库，并聚合为源码构建同名目标：3rd_ffmpeg。
include("${CMAKE_CURRENT_LIST_DIR}/PrebuiltLayout.cmake")
# 引擎包根可显式覆盖，但不能自动转到外部系统 FFmpeg 或源码依赖。
ice_prebuilt_init("${CMAKE_CURRENT_LIST_DIR}/../../prebuilts")
if(ICE_LINKAGE STREQUAL "static")
  # 归档不携带外部编码器和压缩库的实现，必须把它们补入最终链接闭包。 shared 模式由 FFmpeg 动态库自身引用这些依赖，不在聚合目标重复展开。
  # LAME 独立导入，不能仅凭 avcodec 文件存在就判定静态包完整。
  find_package(lame REQUIRED)
  # 容器压缩等实现依赖 zlib，使用具体包目标继承其配置和头路径。
  find_package(zlib REQUIRED)
endif()
# 共享头目录只校验存在性，组件 ABI 和可选编解码能力需由构建产物验证。
ice_prebuilt_include_dir(_ffmpeg_include_dir ffmpeg)
# 所有组件共享同一头文件根，版本一致性由预编译包制作者保证。

foreach(_ffmpeg_component avformat avcodec swscale swresample avutil)
  # 每个组件显式建立稳定的命名空间目标，不把一串裸路径直接泄漏给业务层。 组件顺序先列上层容器和编解码，再列转换与基础工具，便于静态链接解析。
  if(NOT TARGET FFmpeg::${_ffmpeg_component})
    # 已存在组件不覆盖其配置，避免重入时改变上层工程提供的导入定义。
    add_library(FFmpeg::${_ffmpeg_component} UNKNOWN IMPORTED GLOBAL)
    set_target_properties(
      FFmpeg::${_ffmpeg_component} PROPERTIES INTERFACE_INCLUDE_DIRECTORIES
                                              "${_ffmpeg_include_dir}")

    ice_prebuilt_target_configs(_ffmpeg_configs)
    # 当前组件独立累积配置，避免前一组件的默认库路径串入下一组件。
    set(_ffmpeg_imported_configs "")
    # 当前组件清空通用位置，循环之间不会复用其他组件的链接文件。
    set(_ffmpeg_default_library "")
    foreach(_ffmpeg_config IN LISTS _ffmpeg_configs)
      # shared 运行时登记使用全局清单；这里不为不同配置另外创建部署目标。 属性使用请求配置名称，实际目录可由发布配置映射规则选择带符号包。
      # 磁盘配置和请求属性分开，避免目录大小写影响 CMake 导入属性解析。
      string(TOUPPER "${_ffmpeg_config}" _ffmpeg_config_upper)
      # lib 前缀候选兼容归档命名；helper 限制查找范围，缺任一组件立即失败。 Windows shared 同时验证对应
      # DLL，并登记到后续可执行文件部署列表。
      ice_prebuilt_find_library(_ffmpeg_library ffmpeg "${_ffmpeg_config}"
                                ${_ffmpeg_component} lib${_ffmpeg_component})
      list(APPEND _ffmpeg_imported_configs "${_ffmpeg_config_upper}")
      # 不用首个找到的组件库充当所有配置，Debug 与发布请求保持独立位置。
      set_target_properties(
        FFmpeg::${_ffmpeg_component}
        PROPERTIES "IMPORTED_LOCATION_${_ffmpeg_config_upper}"
                   "${_ffmpeg_library}")
      # 默认位置只解决无配置选择，不用于掩盖其他请求配置的缺失。
      if(_ffmpeg_default_library STREQUAL "")
        # 通用默认位置只从当前组件首个解析结果选取。
        set(_ffmpeg_default_library "${_ffmpeg_library}")
      endif()
    endforeach()
    if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
      # 未指定配置时仍可解析导入位置，查找的默认包按布局规则选择发布配置。
      set_target_properties(
        FFmpeg::${_ffmpeg_component} PROPERTIES IMPORTED_LOCATION_NOCONFIG
                                                "${_ffmpeg_default_library}")
    endif()
    set_target_properties(
      FFmpeg::${_ffmpeg_component}
      # 可用配置列表和通用位置一起设置，保持多配置与单配置消费都可解析。
      PROPERTIES IMPORTED_CONFIGURATIONS "${_ffmpeg_imported_configs}"
                 IMPORTED_LOCATION "${_ffmpeg_default_library}")
  endif()
endforeach()

if(NOT TARGET 3rd_ffmpeg)
  # 聚合目标只转发链接依赖，不重新编译或合并 FFmpeg 二进制。 所有业务模块只消费该稳定入口，不需要知道来源是预编译还是源码构建。
  # 这里只提供列出的五个组件，不表示 avdevice 或 avfilter 也已导入。
  add_library(3rd_ffmpeg INTERFACE)
  target_link_libraries(
    3rd_ffmpeg
    INTERFACE FFmpeg::avformat
              # 编解码和像素转换也用于封面与视频处理，不能只留下音频重采样组件。
              FFmpeg::avcodec FFmpeg::swscale FFmpeg::swresample FFmpeg::avutil)
  # 保持依赖库排在使用它们的 FFmpeg 组件之后，静态归档解析顺序才可闭合。
  if(ICE_LINKAGE STREQUAL "static")
    # 依赖库排在使用它们的 FFmpeg 组件之后，静态链接时保留引用方向。
    target_link_libraries(3rd_ffmpeg INTERFACE 3rd_lame 3rd_zlib)
  endif()
  if(WIN32)
    # 这些系统入口由 FFmpeg 预编译配置决定，并非引擎直接使用每个 Windows API。
    # 明确传播到最终链接目标，避免消费者分别补链而出现配置间差异。
    target_link_libraries(
      3rd_ffmpeg
      INTERFACE bcrypt
                # 窗口与 COM 类型库入口来自平台多媒体和设备模块。
                user32
                ole32
                strmiids
                uuid
                # 网络及安全接口支持所启用的协议和系统证书相关实现。
                ws2_32
                # 系统安全依赖匹配已有预编译功能，不在配置阶段启用新的协议。
                secur32
                ncrypt
                crypt32
                advapi32
                shell32
                # 平台视频和媒体 GUID 依赖需与预编译包启用的后端保持一致。
                vfw32
                # 媒体 GUID 定义由系统库提供，本模块不执行设备探测或媒体处理。
                mfuuid)
  elseif(APPLE)
    # Apple 媒体框架用于容器、视频和音频平台集成，使用框架名交由 SDK 解析。
    target_link_libraries(
      3rd_ffmpeg
      INTERFACE "-framework CoreFoundation"
                "-framework CoreVideo"
                "-framework CoreMedia"
                "-framework AudioToolbox"
                "-framework VideoToolbox"
                "-framework Security"
                # 压缩、数学与字符编码库是此平台归档仍需解析的非框架依赖。
                bz2
                m
                iconv)
  else()
    # Unix 的线程、压缩和动态加载依赖在最终目标上展开，保持归档依赖闭包。
    target_link_libraries(3rd_ffmpeg INTERFACE m pthread lzma bz2 dl)
  endif()
endif()

# 所需组件均已导入后才宣告成功；目录或文件缺失已由 helper 终止配置。 头目录存在不等于所有可选 FFmpeg 功能可用，消费范围限于已导入组件。
set(ffmpeg_FOUND TRUE)
