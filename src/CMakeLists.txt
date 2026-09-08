# src/CMakeLists.txt

# IonCachyEngine 当前源码由 ice/ 目录集中维护，这里保留原有递归收集方式。 只收集引擎实现，不把同级示例或 tests 下的 main
# 一并编进库。 CONFIGURE_DEPENDS 让新增实现文件触发重配置，避免手动刷新源列表。
file(GLOB_RECURSE ICE_SOURCES CONFIGURE_DEPENDS "ice/*.cpp")

# 依赖链接偏好为 shared 时，引擎本体生成平台共享库，Windows 上对应 DLL。 保留已有静态库目标名，避免破坏按目标名构建引擎的脚本。
set(ICE_LIBRARY_TARGET IonCachyEngine-static)
set(ICE_LIBRARY_TYPE STATIC)
if(ICE_LINKAGE STREQUAL "shared")
  # 共享目标使用无 static 后缀的名字，稳定消费入口仍由别名提供。
  set(ICE_LIBRARY_TARGET IonCachyEngine)
  set(ICE_LIBRARY_TYPE SHARED)
endif()

# 目标类型由链接偏好显式确定，不依赖外部 BUILD_SHARED_LIBS 的隐式行为。
add_library(${ICE_LIBRARY_TARGET} ${ICE_LIBRARY_TYPE} ${ICE_SOURCES})
# Windows 共享库输出符号和导入宏必须配套，静态目标不携带导入修饰。
if(ICE_LINKAGE STREQUAL "shared" AND WIN32)
  set_target_properties(${ICE_LIBRARY_TARGET}
                        PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
  # 构建库自身使用导出宏，消费方仅继承共享库使用标志。
  target_compile_definitions(
    ${ICE_LIBRARY_TARGET}
    PRIVATE ICE_BUILDING_LIBRARY
    PUBLIC ICE_SHARED_LIBRARY)
endif()

# 构建树使用源码 include；安装后改用安装前缀内的相对 include 目录。 INSTALL_INTERFACE
# 仅预留消费路径，本文件没有安装头文件或导出目标的规则。
target_include_directories(
  ${ICE_LIBRARY_TARGET} PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
                               $<INSTALL_INTERFACE:include>)

# 公开依赖满足引擎接口及消费方链接需求，具体包布局由独立 Find 脚本负责。 私有实现依赖不应通过业务代码引用其文件系统路径。
target_link_libraries(
  ${ICE_LIBRARY_TARGET}
  PUBLIC 3rd_sdl3 3rd_rubberband
  PRIVATE fmt::fmt spdlog::spdlog OpenAL::OpenAL 3rd_ffmpeg)

# 宿主统一链接此别名，不必根据引擎本体的静态/动态目标名写分支。
add_library(ICE::IonCachyEngine ALIAS ${ICE_LIBRARY_TARGET})

if(ICE_BUILD_TESTS)
  # 示例程序供交互运行；实时测试单独注册到 CTest，不自动运行音频示例。 示例含设备播放流程，只构建它并不表示当前机器的音频设备可用。
  add_executable(testIonCachyEngin main.cpp)
  target_link_libraries(testIonCachyEngin PRIVATE ${ICE_LIBRARY_TARGET}
                                                  fmt::fmt)
  # 回归测试链接当前构建的引擎，不以仓库中的旧引擎预编译包替代。
  add_executable(
    TimeStretcherRealtimeTest
    "${CMAKE_CURRENT_SOURCE_DIR}/../tests/TimeStretcherRealtimeTest.cpp")
  target_link_libraries(TimeStretcherRealtimeTest PRIVATE ${ICE_LIBRARY_TARGET})
  # C 堆包装仅在非 Apple 的 UNIX GNU/Clang 配置启用，Windows Clang64 不在其中。
  if(UNIX
     AND NOT APPLE
     AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    # wrap 只观测最终链接时可重定向的 C 堆符号，不能据此覆盖共享库内部所有分配。
    # 编译宏与链接选项配套，避免测试入口与包装符号不一致；工具链须支持此链接选项。
    target_compile_definitions(TimeStretcherRealtimeTest
                               PRIVATE ICE_TEST_WRAP_MALLOC=1)
    target_link_options(
      TimeStretcherRealtimeTest PRIVATE "-Wl,--wrap=malloc" "-Wl,--wrap=calloc"
      "-Wl,--wrap=realloc" "-Wl,--wrap=free")
  endif()
  # 只登记自动化用例，示例播放器不参与无人值守测试；此处没有设置 CTest 超时。
  add_test(NAME IonTimeStretcherRealtimeTest COMMAND TimeStretcherRealtimeTest)
  # shared 预编译依赖复制到独立测试程序旁边，避免 Windows 运行时查找不到 DLL。 源码依赖模式不一定定义运行时复制函数，因此先检查
  # helper 是否存在。
  if(COMMAND ice_prebuilt_copy_runtime_files)
    # 两个可执行文件分别部署运行时，单独启动测试也能找到动态依赖。
    ice_prebuilt_copy_runtime_files(testIonCachyEngin)
    ice_prebuilt_copy_runtime_files(TimeStretcherRealtimeTest)
  endif()

  message(STATUS "Building IonCachyEngine tests enabled.")
else()
  # 关闭测试仍构建完整引擎库，只跳过示例和回归测试可执行文件。
  message(STATUS "Building IonCachyEngine tests disabled.")
endif()
