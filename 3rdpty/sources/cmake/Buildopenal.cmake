# 配置 OpenAL 源码依赖并补充引擎使用要求；只由 SOURCES_BUILD=ON 入口包含。

# 路径相对于调用目录而非脚本目录，入口移动时须同步检查此相对关系。
set(OPENAL_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../sources/openal")
# 缺源码时由后续子目录配置报错，本文件不负责克隆仓库或寻找系统 OpenAL 替代品。

# 引擎只保留一个库类型，与预编译模式的 ICE_LINKAGE 偏好一致。
set(ICE_OPENAL_LIBTYPE "STATIC")
if(ICE_LINKAGE STREQUAL "shared")
  # shared 仅改变依赖库类型，不在这里决定主程序或引擎本体的库类型。
  set(ICE_OPENAL_LIBTYPE "SHARED")
endif()
# 显式强制上游缓存，避免同一构建树残留的 LIBTYPE 覆盖当前链接偏好。
set(LIBTYPE
    "${ICE_OPENAL_LIBTYPE}"
    CACHE STRING "Force OpenAL Soft library type" FORCE)
# 依赖构建不需要设备诊断工具，避免为工具引入额外可执行目标和依赖。
set(ALSOFT_UTILS
    OFF
    CACHE BOOL "Disable OpenAL utils" FORCE)
# 示例不属于引擎的自动化测试范围，不随依赖库进入默认构建。
set(ALSOFT_EXAMPLES
    OFF
    CACHE BOOL "Disable OpenAL examples" FORCE)
# 关闭上游测试不代表后端行为已验证，ICE 的设备与并发测试需独立完成。
set(ALSOFT_TESTS
    OFF
    CACHE BOOL "Disable OpenAL tests" FORCE)
# 嵌入式依赖不向系统前缀安装，发布预编译包由外部打包流程管理。
set(ALSOFT_INSTALL
    OFF
    CACHE BOOL "Disable OpenAL install target" FORCE)
# 将 HRTF 数据随库携带，减少运行时单独部署该数据的需求，不等于强制启用 HRTF。
set(ALSOFT_EMBED_HRTF_DATA
    ON
    CACHE BOOL "Embed HRTF data into the library" FORCE)

# 禁用配置工具入口；本脚本不直接声明 sndfile、Qt 或它们的查找结果。
set(ALSOFT_NO_CONFIG_UTIL
    ON
    CACHE BOOL "Disable alsoft-config utility" FORCE)
# 避免 OpenAL 再以 SDL2 为输出依赖，与引擎独立的 SDL3 播放器保持分离。
set(ALSOFT_BACKEND_SDL2
    OFF
    CACHE BOOL "Disable SDL2 backend to avoid extra deps" FORCE)
# 引擎可同时构建两种播放器，但 OpenAL 的依赖链不再反向包含 SDL3。
set(ALSOFT_BACKEND_SDL3
    OFF
    CACHE BOOL "Disable SDL3 backend to avoid extra deps" FORCE)
# 这里只禁用所列后端，不能把未列出的平台后端视为已显式开启或关闭。

if(CMAKE_CROSSCOMPILING OR WIN32)
  # 分支覆盖所有交叉编译，不仅 Windows；交叉到 Linux/macOS 也会禁用以下后端。
  # 这是现有裁剪策略，不能据此声称交叉目标保留了全部本地音频服务支持。
  set(ALSOFT_BACKEND_PIPEWIRE
      OFF
      CACHE BOOL "" FORCE)
  # ALSA 不由 Windows 目标链接，交叉到 Linux 若需要它需单独审查这项强制关闭。
  set(ALSOFT_BACKEND_ALSA
      OFF
      CACHE BOOL "" FORCE)
  # 不允许宿主 PulseAudio 探测结果进入目标构建的使用要求。
  set(ALSOFT_BACKEND_PULSEAUDIO
      OFF
      CACHE BOOL "" FORCE)
  # JACK 与 OSS 是另外两种设备路径，不由前面 PipeWire 开关间接控制。
  set(ALSOFT_BACKEND_JACK
      OFF
      CACHE BOOL "" FORCE)
  set(ALSOFT_BACKEND_OSS
      OFF
      CACHE BOOL "" FORCE)
  # PortAudio 封装会再引入一层输出依赖，此分支不将其作为自动后备。
  set(ALSOFT_BACKEND_PORTAUDIO
      OFF
      CACHE BOOL "" FORCE)
  # sndio 同样被强制排除；该配置分支不按交叉目标再细分 BSD 等系统。
  set(ALSOFT_BACKEND_SNDIO
      OFF
      CACHE BOOL "" FORCE)
  # CoreAudio 在交叉到 Apple 时也会关闭，后文链接框架不会重新打开此后端。
  set(ALSOFT_BACKEND_COREAUDIO
      OFF
      CACHE BOOL "" FORCE)
  # Android 的两种后端都不在此裁剪集合内保留，不代表此入口已支持 Android 播放。
  set(ALSOFT_BACKEND_OBOE
      OFF
      CACHE BOOL "" FORCE)
  set(ALSOFT_BACKEND_OPENSL
      OFF
      CACHE BOOL "" FORCE)
endif()
# 该分支没有恢复缓存的对称逻辑，跨平台复用构建目录会遗留 OFF 值，应隔离构建树。

# 上游在此创建真实 OpenAL 目标，选项必须在 add_subdirectory 前设置才参与配置。 EXCLUDE_FROM_ALL
# 不阻止被链接的库构建；SYSTEM 用于隔离上游包含目录的诊断。
add_subdirectory(${OPENAL_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR}/openal_build
                 EXCLUDE_FROM_ALL SYSTEM)

# 补充消费侧系统链接项，不取代上游 OpenAL 目标已有的依赖闭包。
if(APPLE)
  # 框架作为 INTERFACE 要求传播到消费方，不在这里创建或初始化音频设备。
  target_link_libraries(
    OpenAL INTERFACE "-framework CoreFoundation" "-framework CoreAudio"
                     "-framework AudioToolbox")
elseif(WIN32)
  # Windows 计时/多媒体与 COM 入口由系统库提供，避免静态消费方遗漏解析项。
  target_link_libraries(OpenAL INTERFACE winmm ole32)
else()
  # 此分支只显式补 dl，不额外添加 pthread，也不对所有非 Apple/Windows 系统做兼容保证。
  target_link_libraries(OpenAL INTERFACE dl)
endif()

# PIC 对静态库也启用，允许它被链接进共享产物；不改变上面的库类型选择。
set_target_properties(OpenAL PROPERTIES POSITION_INDEPENDENT_CODE ON)
# 消费侧宏与实际库类型必须一起变化；静态内部符号入口不能透传给 DLL 消费者。
if(ICE_LINKAGE STREQUAL "static")
  # 静态 OpenAL Soft 可以解析内部日志回调符号；动态 DLL 不公开该非稳定符号。
  target_compile_definitions(OpenAL INTERFACE AL_LIBTYPE_STATIC
                                              ICE_OPENALSOFT_STATIC_LINKAGE)
endif()
# 本入口不附加业务 PGO 选项；父级也必须避免将插桩参数以目录级方式传播到第三方。
