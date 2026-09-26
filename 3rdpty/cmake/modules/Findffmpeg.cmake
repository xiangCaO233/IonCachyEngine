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

# 静态 avcodec 引用外部 Vorbis、Ogg 和 Opus 实现，按相同配置导入归档。
if(ICE_LINKAGE STREQUAL "static")
  foreach(_xiph_component vorbisenc vorbis ogg opus)
    if(NOT TARGET Xiph::${_xiph_component})
      add_library(Xiph::${_xiph_component} UNKNOWN IMPORTED GLOBAL)
      ice_prebuilt_target_configs(_xiph_configs)
      # 清空每个组件的通用位置，避免 Vorbis 和 Opus 指向同一个归档。
      set(_xiph_default_library "")
      # 多配置属性必须只包含本组件已经找到的构建类型。
      set(_xiph_imported_configs "")
      foreach(_xiph_config IN LISTS _xiph_configs)
        # 属性名采用大写配置，磁盘上的配置映射由 helper 统一处理。
        string(TOUPPER "${_xiph_config}" _xiph_config_upper)
        # 缺少任一配置的归档时尽早失败，不能用系统库静默填补。
        ice_prebuilt_find_library(_xiph_library xiph "${_xiph_config}"
                                  ${_xiph_component} lib${_xiph_component})
        set_target_properties(
          Xiph::${_xiph_component}
          PROPERTIES "IMPORTED_LOCATION_${_xiph_config_upper}"
                     "${_xiph_library}")
        # 真实路径设置后才宣告该配置可用。
        list(APPEND _xiph_imported_configs "${_xiph_config_upper}")
        if(_xiph_default_library STREQUAL "")
          set(_xiph_default_library "${_xiph_library}")
        endif()
      endforeach()
      set_target_properties(
        Xiph::${_xiph_component}
        PROPERTIES IMPORTED_CONFIGURATIONS "${_xiph_imported_configs}"
                   IMPORTED_LOCATION "${_xiph_default_library}")
    endif()
  endforeach()
endif()

if(NOT TARGET 3rd_ffmpeg)
  # 聚合目标只转发链接依赖，不重新编译或合并 FFmpeg 二进制。 所有业务模块只消费该稳定入口，不需要知道来源是预编译还是源码构建。
  # 这里只提供列出的五个组件，不表示 avdevice 或 avfilter 也已导入。
  add_library(3rd_ffmpeg INTERFACE)
  target_link_libraries(
    3rd_ffmpeg
    INTERFACE # 文件容器入口先于实际编解码归档。
              FFmpeg::avformat
              # 编解码和像素转换也用于封面与视频处理，不能只留下音频重采样组件。
              FFmpeg::avcodec
              # 视频与封面路径保留像素格式转换能力。
              FFmpeg::swscale
              # 音频导出路径保留采样率与样本格式转换能力。
              FFmpeg::swresample
              # 公共工具库必须在引用它的组件之后解析。
              FFmpeg::avutil)
  # 保持依赖库排在使用它们的 FFmpeg 组件之后，静态归档解析顺序才可闭合。
  if(ICE_LINKAGE STREQUAL "static")
    # 三个平台的静态 FFmpeg 归档均传播相同的 Xiph 链接闭包。
    target_link_libraries(3rd_ffmpeg INTERFACE Xiph::vorbisenc Xiph::vorbis
                                               Xiph::ogg Xiph::opus)
    # 依赖库排在使用它们的 FFmpeg 组件之后，静态链接时保留引用方向。
    target_link_libraries(3rd_ffmpeg INTERFACE 3rd_lame 3rd_zlib)
  endif()
  if(WIN32)
    # 这些系统入口由 FFmpeg 预编译配置决定，并非引擎直接使用每个 Windows API。
    # 明确传播到最终链接目标，避免消费者分别补链而出现配置间差异。
    target_link_libraries(
      3rd_ffmpeg
      INTERFACE bcrypt
                # 窗口与 COM 类型库入口来自平台多媒体和设备模块。 bcrypt 由系统密码 API 提供，不能改为第三方包路径。 窗口与
                # COM 类型库入口来自平台多媒体和设备模块。
                user32
                ole32
                # DirectShow 类型标识符由平台 SDK 归档提供。
                strmiids
                uuid
                # Winsock 对应 FFmpeg 配置启用的网络协议。 网络及安全接口支持所启用的协议和系统证书相关实现。
                ws2_32
                # 系统安全依赖匹配已有预编译功能，不在配置阶段启用新的协议。
                secur32
                # 证书与加密路径由系统库提供最终符号。
                ncrypt
                crypt32
                advapi32
                shell32
                # 视频设备 API 与 Media Foundation 的 GUID 分属不同库。 平台视频和媒体 GUID
                # 依赖需与预编译包启用的后端保持一致。
                vfw32
                # 媒体 GUID 定义由系统库提供，本模块不执行设备探测或媒体处理。
                mfuuid)
  elseif(APPLE)
    # Apple 媒体框架用于容器、视频和音频平台集成，使用框架名交由 SDK 解析。
    target_link_libraries(
      3rd_ffmpeg
      INTERFACE "-framework CoreFoundation"
                # 视频帧缓冲由 CoreVideo 定义，媒体时间由 CoreMedia 定义。
                "-framework CoreVideo"
                "-framework CoreMedia"
                # 音频输出与视频硬件加速使用各自的平台框架。
                "-framework AudioToolbox"
                "-framework VideoToolbox"
                "-framework Security"
                # 压缩与字符集转换仍需明确传给最终链接器。 压缩、数学与字符编码库是此平台归档仍需解析的非框架依赖。
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
