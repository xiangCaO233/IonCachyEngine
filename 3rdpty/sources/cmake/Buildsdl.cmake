# * 此入口为 ICE 的音频输出裁剪 SDL3，不提供通用桌面 UI 所需的完整 SDL 构建。
# * 仅由 SOURCES_BUILD=ON 加载，不能作为预编译模式缺包时的隐式回退。
# * 所有上游缓存强制写入以保持构建可重复；共享缓存的其他消费者不能假定保留自己的 SDL 选项。

# 该路径按调用目录定位，不是 CMAKE_CURRENT_LIST_DIR，入口目录结构须保持一致。
set(SDL_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../sources/sdl")
# 缺源码时不改用系统 SDL；两种依赖来源的切换只由上层 SOURCES_BUILD 控制。

# 静态和动态开关成对设置，只要求上游产出当前链接偏好所需的一种实现。
set(ICE_SDL_BUILD_SHARED OFF)
set(ICE_SDL_BUILD_STATIC ON)
if(ICE_LINKAGE STREQUAL "shared")
  # 动态模式明确关闭静态目标，后文不能再无条件操作 SDL3-static。
  set(ICE_SDL_BUILD_SHARED ON)
  set(ICE_SDL_BUILD_STATIC OFF)
endif()

# 强制刷新上游缓存，避免重新配置时同时残留两种库类型。
set(SDL_SHARED
    ${ICE_SDL_BUILD_SHARED}
    CACHE BOOL "Build SDL3 as a shared library" FORCE)
set(SDL_STATIC
    ${ICE_SDL_BUILD_STATIC}
    CACHE BOOL "Build SDL3 as a static library" FORCE)
# 这里的库类型不替代 MSVC 运行库偏好，CRT 仍须由父级统一配置。

# 上游测试、示例与安装入口都不属于嵌入式音频依赖的交付范围。
set(SDL_TESTS
    OFF
    CACHE BOOL "Disable SDL3 tests" FORCE)
# XTEST 是 X11 输入扩展选项，不应仅因名字含 TEST 就当作 SDL 测试套件开关。
set(SDL_X11_XTEST
    OFF
    CACHE BOOL "Disable SDL3 X11 Xtests" FORCE)
# 禁用示例避免构建裁剪后的库本身不支持的窗口或输入演示程序。
set(SDL_EXAMPLES
    OFF
    CACHE BOOL "Disable SDL3 examples" FORCE)
# 不安装到宿主系统；预编译布局的 headers、libs 与 bin 由打包流程另行组织。
set(SDL_INSTALL
    OFF
    CACHE BOOL "Disable SDL3 install target" FORCE)
# 控制台构建倾向与后面的显式窗口后端裁剪共同生效，不代替真实音频后端选择。
set(SDL_UNIX_CONSOLE_BUILD
    ON
    CACHE BOOL "Build for console, skip windowing deps" FORCE)

# 保留音频输出与基础调度能力；这是一套项目裁剪策略，不是 SDL 的通用最小配置。
set(SDL_AUDIO
    ON
    CACHE BOOL "Enable Audio subsystem" FORCE)
# 事件能力与图形窗口分离保留，不能因 SDL_VIDEO=OFF 就一并删去事件入口。
set(SDL_EVENTS
    ON
    CACHE BOOL "Enable Events subsystem" FORCE)
# 定时基础设施供内部调度使用，并不设置 ICE 的播放缓冲或更新频率。
set(SDL_TIMERS
    ON
    CACHE BOOL "Enable Timers subsystem" FORCE)
# 线程支持不能因前台没有图形界面而关闭，音频设备实现仍可能使用工作线程。
set(SDL_THREADS
    ON
    CACHE BOOL "Enable Threads subsystem" FORCE)
# 电源能力属于当前保留集合，不意味着下方所有桌面集成依赖都必须打开。
set(SDL_POWER
    ON
    CACHE BOOL "Enable Power management subsystem" FORCE)
# 该脚本只写入配置，不在 CMake 阶段枚举声卡、启动音频线程或测试设备权限。

# * Linux 保留真实设备输出候选，不只生成可链接但无法接入桌面音频服务的裁剪包。
# * 这些选项表达构建意图，实际后端是否启用还需结合目标头文件与上游配置结果验收。
# * 动态加载服务库不同于 SDL_SHARED：静态 SDL 也可以在运行时加载 PipeWire 等服务库。
if(LINUX)
  # PipeWire 是现代 Linux 桌面默认音频服务，PulseAudio 和 ALSA 作为广泛部署的兼容路径。 动态加载可避免静态 SDL
  # 目标把宿主机音频库路径传播给主项目链接命令。
  set(SDL_PIPEWIRE
      ON
      CACHE BOOL "Enable SDL3 PipeWire audio backend" FORCE)
  # 动态加载把服务库可用性推迟到运行期，不保证用户机器已经安装服务或运行时文件。
  set(SDL_PIPEWIRE_SHARED
      ON
      CACHE BOOL "Dynamically load PipeWire for SDL3 audio" FORCE)
  # 同时保留 PulseAudio 路径，构建时不将 PipeWire 作为唯一运行环境假设。
  set(SDL_PULSEAUDIO
      ON
      CACHE BOOL "Enable SDL3 PulseAudio audio backend" FORCE)
  # 该 SHARED 选项控制服务依赖的加载方式，不是再生成一份 SDL 动态目标。
  set(SDL_PULSEAUDIO_SHARED
      ON
      CACHE BOOL "Dynamically load PulseAudio for SDL3 audio" FORCE)
  # ALSA 提供另一种设备路径，但本脚本不决定运行时后端优先级或默认设备。
  set(SDL_ALSA
      ON
      CACHE BOOL "Enable SDL3 ALSA audio backend" FORCE)
  # 避免最终静态消费者直接绑定配置机器上的 ALSA 库绝对路径。
  set(SDL_ALSA_SHARED
      ON
      CACHE BOOL "Dynamically load ALSA for SDL3 audio" FORCE)
  # JACK 候选也保留，是否有可连接的音频服务由运行环境决定。
  set(SDL_JACK
      ON
      CACHE BOOL "Enable SDL3 JACK audio backend" FORCE)
  # 构建成功不等于 JACK 服务可用，设备播放验证不能只以链接通过代替。
  set(SDL_JACK_SHARED
      ON
      CACHE BOOL "Dynamically load JACK for SDL3 audio" FORCE)
  # Linux 条件描述目标系统；交叉编译时对应头文件和库探测必须来自目标工具链。
endif()
# 非 Linux 分支不清空上述缓存，因此跨目标复用构建树仍可能保留旧值，应隔离工具链构建目录。

# 关闭窗口、渲染、相机和输入外设功能，避免它们间接扩大音频依赖的系统链接闭包。
set(SDL_VIDEO
    OFF
    CACHE BOOL "Disable Video subsystem" FORCE)
# SDL_RENDER 与底层窗口独立列出，防止继承的上游缓存重新开启渲染器。
set(SDL_RENDER
    OFF
    CACHE BOOL "Disable Render subsystem" FORCE)
# GPU 接口不服务当前音频播放器，不应因主项目使用图形 API 而在此开启。
set(SDL_GPU
    OFF
    CACHE BOOL "Disable GPU subsystem" FORCE)
# 摄像头采集不属于 ICE 的音频设备范围，不据主程序是否需要相机来扩展此依赖。
set(SDL_CAMERA
    OFF
    CACHE BOOL "Disable Camera subsystem" FORCE)
# 游戏控制器与音频设备分属不同子系统，裁剪控制器不应解释为关闭音频热插拔。
set(SDL_JOYSTICK
    OFF
    CACHE BOOL "Disable Joystick subsystem" FORCE)
# 触觉反馈通常服务输入外设，此构建不把它当作音频输出振动接口。
set(SDL_HAPTIC
    OFF
    CACHE BOOL "Disable Haptic subsystem" FORCE)
# 传感器采集与 PCM 供数链路无关，保持显式关闭以约束上游功能范围。
set(SDL_SENSOR
    OFF
    CACHE BOOL "Disable Sensor subsystem" FORCE)
# HIDAPI 的外设访问不用于当前 SDL 播放器接口，避免带入另一组设备依赖。
set(SDL_HIDAPI
    OFF
    CACHE BOOL "Disable HIDAPI subsystem" FORCE)
# 托盘集成归宿主 UI 管理，音频依赖不另行创建桌面托盘能力。
set(SDL_TRAY
    OFF
    CACHE BOOL "Disable System Tray support" FORCE)
# 文件对话框同样由上层负责，不让音频库隐式依赖桌面门户或窗口工具。
set(SDL_DIALOG
    OFF
    CACHE BOOL "Disable Dialog support" FORCE)

# 显式关闭窗口与图形后端，既约束探测范围，也覆盖旧构建缓存中的后端开关。
set(SDL_X11
    OFF
    CACHE BOOL "Disable X11 support" FORCE)
# Wayland 与 X11 分别控制，禁用一种不会自动消除另一套客户端依赖。
set(SDL_WAYLAND
    OFF
    CACHE BOOL "Disable Wayland support" FORCE)
# 主项目使用 Vulkan 不要求 SDL 提供窗口表面集成，保持音频层与图形层分离。
set(SDL_VULKAN
    OFF
    CACHE BOOL "Disable Vulkan support" FORCE)
# 桌面 OpenGL 和 OpenGL ES 分开关闭，避免只关闭其中一种留下图形加载入口。
set(SDL_OPENGL
    OFF
    CACHE BOOL "Disable OpenGL support" FORCE)
set(SDL_OPENGLES
    OFF
    CACHE BOOL "Disable OpenGL ES support" FORCE)
# 控制台构建也不需要直接显示输出，KMS/DRM 不能因跳过窗口系统而自动补上。
set(SDL_KMSDRM
    OFF
    CACHE BOOL "Disable KMS/DRM support" FORCE)
# D-Bus 桌面集成在此裁剪；不代表 Linux 音频服务依赖的所有运行期通信都被移除。
set(SDL_DBUS
    OFF
    CACHE BOOL "Disable D-Bus support" FORCE)
# 音频依赖不承接文本输入法交互，IBus 与事件基础能力独立关闭。
set(SDL_IBUS
    OFF
    CACHE BOOL "Disable IBus (input method) support" FORCE)
# 禁用 libudev 是此包的构建选择，不能由此推导所有音频后端的设备变更行为。
set(SDL_LIBUDEV
    OFF
    CACHE BOOL "Disable libudev support" FORCE)

# * 上述缓存必须先于上游配置设置，add_subdirectory 才会据此创建真实库目标。
# * 单独的二进制目录隔离生成文件；EXCLUDE_FROM_ALL 不阻止被依赖目标按需构建。
# * SYSTEM 用于上游包含目录诊断属性，不意味着上游代码已经过 ICE 注释审计。
add_subdirectory(${SDL_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR}/sdl_build
                 EXCLUDE_FROM_ALL SYSTEM)

# 稳定包装名与预编译模式一致，业务消费者不必区分上游导出的具体 target 名。
add_library(3rd_sdl3 INTERFACE)
if(ICE_LINKAGE STREQUAL "shared")
  if(TARGET SDL3-shared)
    # 优先选择明确的共享目标，不在此分支回退链接静态实现。
    target_link_libraries(3rd_sdl3 INTERFACE SDL3-shared)
  elseif(TARGET SDL3::SDL3)
    # 接受命名空间聚合入口；共享类型依赖前面上游缓存的配置，而不是名字本身证明。
    target_link_libraries(3rd_sdl3 INTERFACE SDL3::SDL3)
  elseif(TARGET SDL3)
    # 最后兼容非命名空间入口，不额外制造别名或另一份 SDL 实现。
    target_link_libraries(3rd_sdl3 INTERFACE SDL3)
  else()
    # 缺少任何受支持的共享入口立即失败，不让无实现的包装目标继续配置成功。
    message(FATAL_ERROR "SDL3 shared target not found.")
  endif()
else()
  # 静态分支要求上游确实提供 SDL3-static；PIC 允许归档以后参与共享库链接。
  set_target_properties(SDL3-static PROPERTIES POSITION_INDEPENDENT_CODE ON)
  target_link_libraries(3rd_sdl3 INTERFACE SDL3-static)
endif()
# 明确传播源码公共头路径，生成头等其他使用要求仍随上游目标的链接接口传递。
target_include_directories(
  3rd_sdl3 INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/../sources/sdl/include")
# 这里不添加业务 PGO 参数；父级不得以全目录编译选项把第三方目标一起插桩。
