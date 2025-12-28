# src/CMakeLists.txt

# 自动搜集源文件 手动列出更清晰，但 GLOB 也可以
file(GLOB_RECURSE ICE_SOURCES "ice/*.cpp")

# 定义核心引擎库 (静态库)
add_library(IonCachyEngine-static STATIC ${ICE_SOURCES})

# 设置 Target 属性
target_include_directories(
  IonCachyEngine-static # PUBLIC: 任何链接 IonCachyEngine 的项目都会自动获得这个 include 路径
  PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include> # 项目内构建时
         $<INSTALL_INTERFACE:include> # 安装后
)

# 将所有第三方依赖链接到引擎库 PRIVATE: 这些依赖是 IonCachyEngine 的实现细节，不应传播给最终用户
target_link_libraries(
  IonCachyEngine-static PRIVATE fmt::fmt spdlog::spdlog OpenAL::OpenAL
                                SDL3::SDL3 3rd_ffmpeg 3rd_rubberband)

# 创建命名空间别名 这使得其他项目可以用更清晰的方式链接
add_library(ICE::IonCachyEngine ALIAS IonCachyEngine-static)

# 可选地构建测试程序
if(ICE_BUILD_TESTS)
    add_executable(testIonCachyEngin main.cpp)

    # 将引擎库链接到测试程序
    target_link_libraries(testIonCachyEngin PRIVATE IonCachyEngine-static)

    message(STATUS "Building 'testIonCachyEngin' enabled.")
else()
    message(STATUS "Building 'testIonCachyEngin' disabled.")
endif()
