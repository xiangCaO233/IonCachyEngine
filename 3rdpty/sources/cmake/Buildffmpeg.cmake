# 3rdpty/sources/cmake/Buildffmpeg.txt
include(ExternalProject)

# 定义安装路径变量
set(FFMPEG_INSTALL_DIR "${CMAKE_CURRENT_BINARY_DIR}/ffmpeg_install")
set(FFMPEG_INCLUDE_DIR "${FFMPEG_INSTALL_DIR}/include")
set(FFMPEG_LIB_DIR "${FFMPEG_INSTALL_DIR}/lib")

# 初始化标志变量
set(FFMPEG_EXTRA_C_FLAGS "")
set(FFMPEG_EXTRA_AS_FLAGS "")

# 平台特定的执行命令构造 ExternalProject_Add 的 WORKING_DIRECTORY 默认是编译目录

# 需要指向源码目录里的 configure 脚本
set(FFMPEG_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/ffmpeg")

# 跨平台编译器标志构造
set(ICE_FFMPEG_CFLAGS "")

if(MSVC)

else()
    set(ICE_FFMPEG_CFLAGS "-march=native")
endif()

# 定义 FFmpeg 编译参数 (使用 LIST 格式，避免空格引起的引号问题)
set(FFMPEG_CONF_LIST
    --prefix=${FFMPEG_INSTALL_DIR}
    --arch=${FFMPEG_ARCH} # 自动探测的结果
    --target-os=${FFMPEG_TARGET_OS} # 自动探测的结果
    --disable-all
    --disable-autodetect
    --enable-avcodec
    --enable-avformat
    --enable-swresample
    --enable-avutil
    --disable-programs
    --disable-doc
    --enable-static
    --disable-shared
    --enable-pic # FFmpeg 内部会自动处理一部分 PIC 逻辑
    --enable-decoder=flac,mp3,aac,vorbis,opus,pcm_s16le,pcm_s24le,pcm_f32le
    --enable-demuxer=flac,mp3,mov,ogg,wav,matroska
    --enable-parser=flac,mpegaudio,aac,vorbis,opus
    --enable-protocol=file
    "--extra-cflags=${ICE_FFMPEG_CFLAGS}"
    "--extra-cxxflags=${ICE_FFMPEG_CFLAGS}"
    "--extra-ldflags=${ICE_FFMPEG_CFLAGS}" # LDFLAGS 也需要 PIC
)

# 调试标志 (直接 append 到 list)
if(CMAKE_BUILD_TYPE STREQUAL "Debug" OR CMAKE_BUILD_TYPE STREQUAL
                                        "RelWithDebInfo")
    list(APPEND FFMPEG_CONF_LIST "--enable-debug" "--disable-stripping")
else()
    list(APPEND FFMPEG_CONF_LIST "--disable-debug" "--enable-stripping")
endif()

# 构造最终命令 (不要带引号展开变量)
if(WIN32)
    set(FFMPEG_CONFIGURE_CMD sh.exe ${FFMPEG_SOURCE_DIR}/configure ${FFMPEG_CONF_LIST})
else()
    set(FFMPEG_CONFIGURE_CMD ${FFMPEG_SOURCE_DIR}/configure ${FFMPEG_CONF_LIST})
endif()

# 动态库后缀处理
if(MSVC)
    set(LIB_EXT ".lib")
    set(LIB_PREFIX "")
else()
    set(LIB_EXT ".a")
    set(LIB_PREFIX "lib")
endif()

# 定义产物路径 (用于 BUILD_BYPRODUCTS，对 Ninja 很有用)
set(FFMPEG_BYPRODUCTS
    ${FFMPEG_LIB_DIR}/${LIB_PREFIX}avformat${LIB_EXT}
    ${FFMPEG_LIB_DIR}/${LIB_PREFIX}avcodec${LIB_EXT}
    ${FFMPEG_LIB_DIR}/${LIB_PREFIX}swresample${LIB_EXT}
    ${FFMPEG_LIB_DIR}/${LIB_PREFIX}avutil${LIB_EXT})

message(STATUS "Debug: FFmpeg Configure Command is")
message(STATUS "${FFMPEG_CONFIGURE_CMD}")

# 执行构建
ExternalProject_Add(
  ffmpeg_project
  SOURCE_DIR ${FFMPEG_SOURCE_DIR}
  CONFIGURE_COMMAND ${FFMPEG_CONFIGURE_CMD}
  # 统一使用 make，Windows MSVC 环境下 FFmpeg 也通常需要适配好的 make (如 Git Bash 里的)
  BUILD_COMMAND make -j${PROCESSOR_COUNT}
  # 执行安装，成功后立刻删除 share 目录，保持 install 目录纯净
  INSTALL_COMMAND make install
  COMMAND ${CMAKE_COMMAND} -E rm -rf ${FFMPEG_INSTALL_DIR}/share
  BUILD_BYPRODUCTS ${FFMPEG_BYPRODUCTS})

# 确保头文件目录存在，防止 CMake 配置阶段报错
file(MAKE_DIRECTORY ${FFMPEG_INCLUDE_DIR})

# --- 封装接口库 ---
set(FFMPEG_LIBS ${FFMPEG_BYPRODUCTS}) # 直接利用上面定义的产物列表

if(WIN32)
    set(FFMPEG_PLATFORM_LIBRARIES
      bcrypt
      user32
      ole32
      strmiids
      uuid
      ws2_32
      secur32
      advapi32
      shell32
      vfw32)
elseif(APPLE)
    set(FFMPEG_PLATFORM_LIBRARIES
      "-framework CoreFoundation"
      "-framework CoreVideo"
      "-framework CoreMedia"
      "-framework AudioToolbox"
      "-framework VideoToolbox"
      "-framework Security"
      bz2
      z
      m
      iconv)
else()
    set(FFMPEG_PLATFORM_LIBRARIES z m pthread lzma bz2 dl)
endif()

add_library(3rd_ffmpeg INTERFACE)
add_dependencies(3rd_ffmpeg ffmpeg_project)
target_include_directories(3rd_ffmpeg INTERFACE ${FFMPEG_INCLUDE_DIR})
target_link_libraries(3rd_ffmpeg INTERFACE ${FFMPEG_LIBS}
                                           ${FFMPEG_PLATFORM_LIBRARIES})