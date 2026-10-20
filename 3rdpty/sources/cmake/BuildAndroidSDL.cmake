# Android 音频与窗口共享同一个 SDL3 动态实例，供 SDLActivity 加载。
# 独立编译时也使用引擎现有源码路径，不依赖上层项目变量。
set(SDL_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/sdl")
# Java JNI 入口要求共享目标；其他音频依赖的链接偏好保持不变。
# Java 外壳与原生共享库必须来自同一个固定 SDL3 版本，不能混用另一个 AAR。
# 此接口保留引擎独立构建能力，不引用主项目的资源或打包脚本。
set(SDL_SHARED
    ON
    CACHE BOOL "Android JNI uses shared SDL3." FORCE)
set(SDL_STATIC
    OFF
    CACHE BOOL "Avoid a second SDL3 instance." FORCE)
# 本平台需要真实的原生窗口与 Vulkan Surface，不能只启用音频子系统。
set(SDL_VIDEO
    ON
    CACHE BOOL "Android window support." FORCE)
# 音频节点与窗口后端共用唯一 SDL 实例，避免两份全局子系统状态。
# 视频能力只在 Android 分支启用，桌面音频构建配置保持原样。
set(SDL_AUDIO
    ON
    CACHE BOOL "Android audio support." FORCE)
# Android 生命周期与输入都依赖事件分发，关闭事件系统会破坏后台恢复。
# ImGui 只消费这一路事件，业务不建立第二套触屏回调。
set(SDL_EVENTS
    ON
    CACHE BOOL "Android input and lifecycle." FORCE)
# Surface 创建使用 SDL Vulkan API，不引入 OpenGL 中间渲染路径。
# 实际 loader 和设备能力在运行期探测，而不是根据宿主 GPU 推定。
set(SDL_VULKAN
    ON
    CACHE BOOL "Android Vulkan surface." FORCE)
# 上游示例与测试不进入应用 APK，也不把安装目录混入源码树。
set(SDL_TESTS
    OFF
    CACHE BOOL "Disable upstream tests." FORCE)
set(SDL_EXAMPLES
    OFF
    CACHE BOOL "Disable upstream examples." FORCE)
set(SDL_INSTALL
    OFF
    CACHE BOOL "No system SDL installation." FORCE)
# 配置输出和生成 JNI 头均写入当前二进制目录，输入源码不被修改。
# 桌面与 Android 的统一 target 名称让音频消费者保持平台无关。
add_subdirectory("${SDL_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/sdl_build"
                 EXCLUDE_FROM_ALL SYSTEM)
# 与桌面和预编译模式暴露相同的消费 target。
add_library(3rd_sdl3 INTERFACE)
target_link_libraries(3rd_sdl3 INTERFACE SDL3::SDL3)
