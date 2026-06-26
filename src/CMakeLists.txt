# src/CMakeLists.txt

# IonCachyEngine 当前源码由 ice/ 目录集中维护，这里保留原有递归收集方式。
file(GLOB_RECURSE ICE_SOURCES CONFIGURE_DEPENDS "ice/*.cpp")

# 依赖链接偏好为 shared 时，IonCachyEngine 本体也生成 DLL，避免主程序吸收音频引擎目标代码。
set(ICE_LIBRARY_TARGET IonCachyEngine-static)
set(ICE_LIBRARY_TYPE STATIC)
if(ICE_LINKAGE STREQUAL "shared")
  set(ICE_LIBRARY_TARGET IonCachyEngine)
  set(ICE_LIBRARY_TYPE SHARED)
endif()

add_library(${ICE_LIBRARY_TARGET} ${ICE_LIBRARY_TYPE} ${ICE_SOURCES})
if(ICE_LINKAGE STREQUAL "shared" AND WIN32)
  set_target_properties(${ICE_LIBRARY_TARGET}
                        PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
  target_compile_definitions(${ICE_LIBRARY_TARGET}
                             PRIVATE ICE_BUILDING_LIBRARY
                             PUBLIC ICE_SHARED_LIBRARY)
endif()

target_include_directories(
  ${ICE_LIBRARY_TARGET}
  PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:include>)

target_link_libraries(
  ${ICE_LIBRARY_TARGET}
  PUBLIC 3rd_sdl3 3rd_rubberband
  PRIVATE fmt::fmt spdlog::spdlog OpenAL::OpenAL 3rd_ffmpeg)

add_library(ICE::IonCachyEngine ALIAS ${ICE_LIBRARY_TARGET})

if(ICE_BUILD_TESTS)
  add_executable(testIonCachyEngin main.cpp)
  target_link_libraries(testIonCachyEngin PRIVATE ${ICE_LIBRARY_TARGET}
                                                  fmt::fmt)

  message(STATUS "Building 'testIonCachyEngin' enabled.")
else()
  message(STATUS "Building 'testIonCachyEngin' disabled.")
endif()
