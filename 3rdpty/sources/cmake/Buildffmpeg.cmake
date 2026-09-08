# * FFmpeg 源码构建适配与依赖接口。
# * 仅由 SOURCES_BUILD=ON 入口加载，不作为预编译模式缺包的隐式回退。
# * 调用入口须先建立 zlib_project、lame_project，并提供架构探测变量。
# * 此文件维护构建适配与目标接口，不修改 FFmpeg 上游源码。
include(ExternalProject)

# * 定义安装路径变量
# * 安装前缀属于可再生构建目录，缓存失效时会整体删除，不能混放用户资源。
# * 发布预编译布局的架构、工具链与配置目录转换不在本入口完成。
set(FFMPEG_INSTALL_DIR "${CMAKE_CURRENT_BINARY_DIR}/ffmpeg_install")
set(FFMPEG_INCLUDE_DIR "${FFMPEG_INSTALL_DIR}/include")
set(FFMPEG_LIB_DIR "${FFMPEG_INSTALL_DIR}/lib")

# * 初始化标志变量
# * 这两个历史扩展变量当前没有进入最终命令，不能把赋值当作已生效参数。
set(FFMPEG_EXTRA_C_FLAGS "")
set(FFMPEG_EXTRA_AS_FLAGS "")

# 平台特定的执行命令构造 ExternalProject_Add 的 WORKING_DIRECTORY 默认是编译目录

# * 需要指向源码目录里的 configure 脚本
# * 源码路径相对于调用目录而非脚本目录，调整 include 入口时须核对位置。
# * configure 来自源码树，ExternalProject 步骤在独立构建目录执行。
set(FFMPEG_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/ffmpeg")

# 跨平台编译器标志构造
set(ICE_FFMPEG_CFLAGS "")
set(ICE_FFMPEG_LDFLAGS "")
set(ICE_FFMPEG_TOOLCHAIN_FLAGS "")
# * 目标 triple 同时供非 MSVC 编译和链接探针使用，不能仅凭驱动路径判断 ABI。
if(CMAKE_C_COMPILER_TARGET)
  string(APPEND ICE_FFMPEG_TOOLCHAIN_FLAGS
         " --target=${CMAKE_C_COMPILER_TARGET}")
endif()
# * sysroot 约束目标头与链接路径，但不自动隔离 pkg-config 的宿主元数据。
if(CMAKE_SYSROOT)
  string(APPEND ICE_FFMPEG_TOOLCHAIN_FLAGS " --sysroot=${CMAKE_SYSROOT}")
endif()
string(STRIP "${ICE_FFMPEG_TOOLCHAIN_FLAGS}" ICE_FFMPEG_TOOLCHAIN_FLAGS)
# * 只提供已配置的 zlib 元数据目录；LAME 通过显式头与库参数参加探测。
# * 配置命令会同时设置 PKG_CONFIG_PATH 和 PKG_CONFIG_LIBDIR，收窄默认搜索范围。
set(ICE_FFMPEG_PKG_CONFIG_PATH "${ICE_ZLIB_PKGCONFIG_DIR}")
# * PATH 在 CMake 配置时取快照，不会自动跟随后续终端的环境修改。
# * 分隔符依据宿主平台选择，与目标是否 Windows 是两件不同的事。
set(ICE_FFMPEG_TOOL_PATH "$ENV{PATH}")
set(ICE_FFMPEG_LIST_SEPARATOR "__ICE_FFMPEG_LIST_SEPARATOR__")
include("${PROJECT_SOURCE_DIR}/cmake/ICEMsvcExternalEnvironment.cmake")
# FFmpeg 会直接调用链接器，三个外部步骤都必须携带 Windows SDK 环境。
ice_msvc_external_environment(ICE_FFMPEG_ENV "${ICE_FFMPEG_LIST_SEPARATOR}")
set(ICE_FFMPEG_TOOL_PATH_SEPARATOR ":")
if(CMAKE_HOST_WIN32)
  # Windows PATH 使用分号分隔，先用唯一占位符保护 CMake 列表边界；同时改用正斜杠，避免末尾反斜杠破坏 sh 引号。
  string(REPLACE "\\" "/" ICE_FFMPEG_TOOL_PATH "${ICE_FFMPEG_TOOL_PATH}")
  string(REPLACE ";" "${ICE_FFMPEG_LIST_SEPARATOR}" ICE_FFMPEG_TOOL_PATH
                 "${ICE_FFMPEG_TOOL_PATH}")
  set(ICE_FFMPEG_TOOL_PATH_SEPARATOR "${ICE_FFMPEG_LIST_SEPARATOR}")
endif()
# * 默认直接采用父工具链驱动，交叉 MSVC 分支才替换参数适配包装器。
set(ICE_FFMPEG_CC "${CMAKE_C_COMPILER}")
set(ICE_FFMPEG_CXX "${CMAKE_CXX_COMPILER}")
set(ICE_FFMPEG_AR "${CMAKE_AR}")

if(MSVC)
  # * 包装器翻译 GCC 风格探针参数，目标对象仍使用 MSVC ABI。
  # * PROJECT_SOURCE_DIR 必须指向 ICE，不能由嵌入方式改变后误指主项目。
  # * 编译器与归档器包装需配套，避免把 Unix ar 参数直接交给 llvm-lib。
  if(CMAKE_CROSSCOMPILING)
    set(ICE_FFMPEG_CC
        "${PROJECT_SOURCE_DIR}/cmake/cross/clang-cl-gcc-compatible.sh")
    set(ICE_FFMPEG_CXX "${ICE_FFMPEG_CC}")
    set(ICE_FFMPEG_AR
        "${PROJECT_SOURCE_DIR}/cmake/cross/llvm-lib-ar-compatible.sh")
    set(ICE_FFMPEG_TOOL_PATH
        "${PROJECT_SOURCE_DIR}/cmake/cross${ICE_FFMPEG_TOOL_PATH_SEPARATOR}${ICE_FFMPEG_TOOL_PATH}"
    )
  endif()
  # * MSVC 参数固定静态 CRT，不根据 ICE_LINKAGE 或父级运行库偏好推导。
  # * 精确 Debug 外统一使用发布优化，/Z7 在两套参数中都保留对象符号。
  if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(ICE_FFMPEG_MSVC_FLAGS "/MTd /Z7 /Od")
  else()
    set(ICE_FFMPEG_MSVC_FLAGS "/MT /Z7 /O2")
  endif()
  # * MSVC 的依赖头来自本次私有安装，不以系统头补齐缺包。
  # * 这里重建参数串，不直接继承父级通用或配置型 C/C++ flags。
  set(ICE_FFMPEG_CFLAGS
      "-I${ICE_ZLIB_INCLUDE_DIR} -I${ICE_LAME_INCLUDE_DIR} ${ICE_FFMPEG_MSVC_FLAGS} -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00"
  )
  set(ICE_FFMPEG_LDFLAGS
      "-libpath:${ICE_ZLIB_LIBRARY_DIR} -libpath:${ICE_LAME_LIBRARY_DIR}")
  # * 这些名字用于 configure 链接探针，需要前置依赖提供对应兼容归档名。
  set(ICE_FFMPEG_EXTRA_LIBS "mp3lame.lib libz.lib")
else()
  # * 非 MSVC 显式请求 PIC，以便静态 FFmpeg 日后被链接进共享产物。
  # * 编译参数没有直接拼入父级 C flags，调试与优化主要由 configure 开关控制。
  set(ICE_FFMPEG_CFLAGS
      "${ICE_FFMPEG_TOOLCHAIN_FLAGS} -I${ICE_ZLIB_INCLUDE_DIR} -I${ICE_LAME_INCLUDE_DIR} -fPIC"
  )
  # * 通用 EXE 链接 flags 仍被继承，父级不得在其中混入业务 PGO 插桩。
  # * 配置专属的 EXE 链接 flags 没有单独拼入，不能假定完全继承父配置。
  set(ICE_FFMPEG_LDFLAGS
      "${ICE_FFMPEG_TOOLCHAIN_FLAGS} -L${ICE_ZLIB_LIBRARY_DIR} -L${ICE_LAME_LIBRARY_DIR} -fPIC ${CMAKE_EXE_LINKER_FLAGS}"
  )
  string(STRIP "${ICE_FFMPEG_CFLAGS}" ICE_FFMPEG_CFLAGS)
  string(STRIP "${ICE_FFMPEG_LDFLAGS}" ICE_FFMPEG_LDFLAGS)
  set(ICE_FFMPEG_EXTRA_LIBS "-lmp3lame -lz")
endif()

# * 音频白名单覆盖当前播放器可解码类型，并补齐可用的原生编码器。
# * 白名单描述请求功能，不替代上游配置输出和真实文件解码验证。
# * PCM 只列出三种小端样本格式，不代表覆盖全部位深与端序组合。
set(ICE_FFMPEG_AUDIO_DECODERS
    flac
    mp3
    aac
    vorbis
    opus
    # PCM codec 名包含位深、数值类型与端序，不能仅按音频采样率选择这些名称。
    pcm_s16le
    pcm_s24le
    pcm_f32le)
# * 编码集合与解码集合独立，能读取某 codec 不代表能够导出同一 codec。
# * 原生编码器的可用性和限制仍需实际配置与编码测试确认。
set(ICE_FFMPEG_AUDIO_ENCODERS
    flac
    aac
    vorbis
    opus
    pcm_s16le
    pcm_s24le
    pcm_f32le)

# * MP3 编码使用外部 LAME；既要登记 encoder，也要在 configure 启用该依赖。
list(APPEND ICE_FFMPEG_AUDIO_ENCODERS libmp3lame)

# * 常见 BG 视频格式与嵌入式 GIF 动画：优先编译原生解码器；编码器只启用无外部依赖项。
# * 该包同时服务背景视频和图像读取，不是仅含音频的最小 avcodec 构建。
# * 列表没有额外请求外部视频 codec 库，不能据此承诺硬件加速可用。
set(ICE_FFMPEG_VIDEO_DECODERS
    gif
    # H.264/HEVC 与下方容器清单配合使用，裸码流和封装视频的入口不同。
    h264
    hevc
    mpeg4
    mpeg2video
    vp8
    vp9
    # AV1 项表示请求原生解码能力，没有额外开启外部 AV1 编码器。
    av1
    mjpeg
    png)
# * 视频输出范围小于输入范围，当前没有要求每种视频 decoder 都配套 encoder。
set(ICE_FFMPEG_VIDEO_ENCODERS mpeg4 mjpeg png)

# * 容器、裸码流和 codec 是不同层级，输入需要相关能力共同满足。
# * MOV/Matroska 等通用封装并不会自动扩大上方解码器白名单。
# * 音频容器也需显式列出，不能仅启用 PCM 解码器就假定 WAV 文件可读。
set(ICE_FFMPEG_DEMUXERS
    aac
    avi
    flac
    flv
    gif
    h264
    hevc
    ivf
    m4v
    matroska
    # MOV 家族包含通用媒体封装，但具体文件是否支持仍受流编码和解析器约束。
    mov
    mp3
    mpeg
    mpegts
    # OGG/WAV 只解决封装读取，不能覆盖上方未列出的任意音频 codec。
    ogg
    wav)
# * 输出封装独立于输入清单，导出时还要验证所选编码与容器的合法组合。
# * MOV 与 MP4 分别请求，不能因它们相关就把其中一项视为可随意删除的重复。
# * WebM 封装可用不意味着任何视频编码都能写入 WebM。
set(ICE_FFMPEG_MUXERS
    adts
    avi
    flac
    flv
    m4v
    matroska
    mov
    mp3
    mp4
    # 输出封装可用不负责自动转码，调用方必须选择当前可用的编码器。
    mpeg
    mpegts
    ogg
    wav
    webm)
# * 解析器处理压缩码流边界，不代替容器解复用或解码实现。
# * 解析器名称未必等同于容器名，例如 mpegaudio 与 mp3 分属不同清单。
# * 保留与音视频 codec 对应的解析能力，不从扩展名自动生成这一集合。
set(ICE_FFMPEG_PARSERS
    aac
    aac_latm
    # 解析器与 decoder 独立启用，缺少任一环节都可能使某些来源的流无法处理。
    av1
    flac
    gif
    h264
    hevc
    mjpeg
    mpeg4video
    mpegaudio
    opus
    png
    vorbis
    vp8
    vp9)
# * 位流过滤器做压缩数据封装适配，不处理 PCM 重采样或音频效果。
# * MP4 到 Annex B 的转换项仅表示可调用，不会自动在每条读取路径执行。
# * AV1/VP9 合帧与拆帧分别保留，运行时仍由消费方选择方向。
set(ICE_FFMPEG_BSFS
    aac_adtstoasc
    # AV1 帧合并和拆分不是可互换选项，保留两项供不同封装路径调用。
    av1_frame_merge
    av1_frame_split
    h264_mp4toannexb
    hevc_mp4toannexb
    vp9_superframe
    vp9_superframe_split)

# * 把 CMake 分号列表转成 configure 的逗号参数，保持每个选项的单一参数边界。
# * 音视频清单合并后不再独立传递，最终能力须核对生成后的完整参数。
string(JOIN "," ICE_FFMPEG_DECODER_LIST ${ICE_FFMPEG_AUDIO_DECODERS}
       ${ICE_FFMPEG_VIDEO_DECODERS})
string(JOIN "," ICE_FFMPEG_ENCODER_LIST ${ICE_FFMPEG_AUDIO_ENCODERS}
       ${ICE_FFMPEG_VIDEO_ENCODERS})
# * 容器、解析器、位流过滤器分别生成参数，不混淆各自的功能命名空间。
string(JOIN "," ICE_FFMPEG_DEMUXER_LIST ${ICE_FFMPEG_DEMUXERS})
string(JOIN "," ICE_FFMPEG_MUXER_LIST ${ICE_FFMPEG_MUXERS})
string(JOIN "," ICE_FFMPEG_PARSER_LIST ${ICE_FFMPEG_PARSERS})
string(JOIN "," ICE_FFMPEG_BSF_LIST ${ICE_FFMPEG_BSFS})

# * 定义 FFmpeg 编译参数 (使用 LIST 格式，避免空格引起的引号问题)
# * 列表保留普通参数边界，但后续还会转成 sh -c 字符串，不保证任意特殊字符安全。
# * 架构与 OS 来自目标探测，不是执行 configure 的宿主系统。
# * 驱动可能是包装器，不一定与父编译器可执行路径逐字相同。
set(FFMPEG_CONF_LIST
    --prefix=${FFMPEG_INSTALL_DIR}
    --arch=${FFMPEG_ARCH} # 自动探测的结果
    --target-os=${FFMPEG_TARGET_OS} # 自动探测的结果
    # --- 指定与顶层项目相同的编译器和工具链 ---
    --cc=${ICE_FFMPEG_CC}
    --cxx=${ICE_FFMPEG_CXX}
    --ar=${ICE_FFMPEG_AR}
    # NM 和 RANLIB 使用父工具链路径，不从修改后的 PATH 随意挑选同名宿主程序。
    --nm=${CMAKE_NM}
    --ranlib=${CMAKE_RANLIB}
    # * -----------------------------------
    # * 从关闭全部功能开始启用清单，避免依赖能力随宿主已安装库任意扩大。
    # * 禁用 autodetect 与显式开启 zlib/LAME 并存，不意味着完全不运行依赖探针。
    --disable-all
    --disable-autodetect
    --enable-avcodec
    # avformat 负责容器与协议层，不能只构建 avcodec 就满足完整文件读写接口。
    --enable-avformat
    # * 重采样与图像缩放属于独立组件，swscale 服务本包同时提供的视频能力。
    --enable-swresample
    --enable-swscale
    # avutil 为其他组件提供公共基础设施，单独列为消费产物而非合并进别的归档。
    --enable-avutil
    --enable-zlib
    # * 不生成 ffmpeg/ffprobe 等 CLI 或文档，调用方通过库 API 使用功能。
    --disable-programs
    --disable-doc
    # * 当前入口固定静态 FFmpeg，不根据 ICE_LINKAGE 产生 DLL 或导入库。
    # * PIC 允许静态对象进入共享产物，但不会改变 FFmpeg 自身的静态构建类型。
    --enable-static
    --disable-shared
    --enable-pic # FFmpeg 内部会自动处理一部分 PIC 逻辑
    # * 依赖探测按静态模式展开，最终目标也需要传播完整外部与系统链接闭包。
    --pkg-config-flags=--static
    --enable-decoder=${ICE_FFMPEG_DECODER_LIST}
    --enable-encoder=${ICE_FFMPEG_ENCODER_LIST}
    --enable-demuxer=${ICE_FFMPEG_DEMUXER_LIST}
    # 输入与输出配置各自传参，防止新增读取格式时无意扩大导出支持承诺。
    --enable-muxer=${ICE_FFMPEG_MUXER_LIST}
    --enable-parser=${ICE_FFMPEG_PARSER_LIST}
    # 位流转换的编译开关不会自动向 ICE 节点图中插入转换节点。
    --enable-bsf=${ICE_FFMPEG_BSF_LIST}
    # * 仅显式开启本地 file 协议，平台链接列表出现套接字库不等于启用 HTTP 等协议。
    --enable-protocol=file
    "--extra-cflags=${ICE_FFMPEG_CFLAGS}"
    # * C++ 扩展参数复用 C 参数串，没有另行继承 CMAKE_CXX_FLAGS。
    "--extra-cxxflags=${ICE_FFMPEG_CFLAGS}"
    "--extra-ldflags=${ICE_FFMPEG_LDFLAGS}" # LDFLAGS 也需要 PIC
    "--extra-libs=${ICE_FFMPEG_EXTRA_LIBS}")
list(APPEND FFMPEG_CONF_LIST --enable-libmp3lame)
# * 前置架构脚本可提供 --toolchain=msvc 等参数，无值时不追加空选项。
if(FFMPEG_ADDITIONAL_CONF)
  list(APPEND FFMPEG_CONF_LIST ${FFMPEG_ADDITIONAL_CONF})
endif()
if(CMAKE_CROSSCOMPILING)
  # FFmpeg 的 configure 需要显式确认交叉构建，否则会把无法运行目标 exe 误判为编译器不可用。
  list(APPEND FFMPEG_CONF_LIST --enable-cross-compile)
endif()

# * 配置名按精确字符串比较，自定义配置不自动获得含符号发布的保留策略。
# * Debug 与 RelWithDebInfo 禁止剥离；普通发布允许剥离，不能混作含符号包验收。 调试标志 (直接 append 到 list)
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  list(APPEND FFMPEG_CONF_LIST "--enable-debug" "--disable-stripping"
       "--disable-optimizations")
elseif(CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
  list(APPEND FFMPEG_CONF_LIST "--enable-debug" "--disable-stripping")
else()
  list(APPEND FFMPEG_CONF_LIST "--disable-debug" "--enable-stripping")
endif()

# * Windows 宿主显式通过 sh.exe 执行脚本，非 Windows 宿主按脚本解释器执行。
# * PKG_CONFIG_LIBDIR 负责收窄默认包目录，仍需验证目标头和库的 ABI 是否匹配。 构造最终命令 (不要带引号展开变量)。这里必须判断宿主
#   shell；Linux 交叉构建的目标平台同样是 WIN32，但不能调用 Windows/MSYS 的 sh.exe。
if(CMAKE_HOST_WIN32)
  set(FFMPEG_CONFIGURE_CMD
      ${CMAKE_COMMAND}
      -E
      env
      ${ICE_FFMPEG_ENV}
      "PATH=${ICE_FFMPEG_TOOL_PATH}"
      "PKG_CONFIG_PATH=${ICE_FFMPEG_PKG_CONFIG_PATH}"
      "PKG_CONFIG_LIBDIR=${ICE_FFMPEG_PKG_CONFIG_PATH}"
      sh.exe
      ${FFMPEG_SOURCE_DIR}/configure
      ${FFMPEG_CONF_LIST})
else()
  set(FFMPEG_CONFIGURE_CMD
      ${CMAKE_COMMAND}
      -E
      env
      ${ICE_FFMPEG_ENV}
      "PATH=${ICE_FFMPEG_TOOL_PATH}"
      "PKG_CONFIG_PATH=${ICE_FFMPEG_PKG_CONFIG_PATH}"
      "PKG_CONFIG_LIBDIR=${ICE_FFMPEG_PKG_CONFIG_PATH}"
      ${FFMPEG_SOURCE_DIR}/configure
      ${FFMPEG_CONF_LIST})
endif()

# * 此处处理静态归档命名，不是 DLL 或共享对象的后缀适配。
# * MinGW 和 Unix 都使用 lib*.a，但归档内部对象格式仍由目标工具链决定。 静态归档的平台命名。
if(MSVC)
  set(LIB_EXT ".lib")
  set(LIB_PREFIX "")
else()
  set(LIB_EXT ".a")
  set(LIB_PREFIX "lib")
endif()

# * 定义产物路径 (用于 BUILD_BYPRODUCTS，对 Ninja 很有用)
# * 登记五个实际消费库，Ninja 可据此连接外部项目与最终链接依赖。
# * 未登记 CLI、头文件或其他非链接产物，清单不能代表整个安装前缀内容。
set(FFMPEG_BYPRODUCTS
    ${FFMPEG_LIB_DIR}/${LIB_PREFIX}avformat${LIB_EXT}
    ${FFMPEG_LIB_DIR}/${LIB_PREFIX}avcodec${LIB_EXT}
    ${FFMPEG_LIB_DIR}/${LIB_PREFIX}swscale${LIB_EXT}
    ${FFMPEG_LIB_DIR}/${LIB_PREFIX}swresample${LIB_EXT}
    ${FFMPEG_LIB_DIR}/${LIB_PREFIX}avutil${LIB_EXT})

# * 哈希仅覆盖 configure 参数列表，不覆盖 PATH、pkg-config 环境或驱动二进制内容。
# * 前置 zlib/LAME 库内容变化也不属于哈希输入，依赖目标完成不保证此戳失效。
string(SHA256 ICE_FFMPEG_CONFIG_HASH "${FFMPEG_CONF_LIST}")
set(FFMPEG_CONFIG_STAMP
    "${FFMPEG_INSTALL_DIR}/.ice_ffmpeg_${ICE_FFMPEG_CONFIG_HASH}.stamp")
# * 源码只比较文件 mtime，不识别文件删除、保留时间的替换或已安装库丢失。
# * 检查依赖 POSIX find/grep，即使目标是 Windows 也需宿主提供对应工具。
set(FFMPEG_SOURCE_READY_TEST
    "test -f '${FFMPEG_CONFIG_STAMP}' && ! /usr/bin/find '${FFMPEG_SOURCE_DIR}' -type f -newer '${FFMPEG_CONFIG_STAMP}' ! -path '*/.git/*' -print -quit | /usr/bin/grep -q ."
)

# * 这里只显示将执行的命令，不能把配置日志打印当作构建或依赖探测已成功。
message(STATUS "Debug: FFmpeg Configure Command is")
message(STATUS "${FFMPEG_CONFIGURE_CMD}")

# 转换为字符串以便在 sh -c 中使用
set(FFMPEG_CONFIGURE_CMD_STR "")
# * 逐项套双引号只处理普通空格，没有进一步转义内嵌双引号、美元符号等内容。
# * 目录和 flags 含 shell 特殊字符时需额外审查，列表形式本身不能消除这一风险。
foreach(arg IN LISTS FFMPEG_CONFIGURE_CMD)
  set(FFMPEG_CONFIGURE_CMD_STR "${FFMPEG_CONFIGURE_CMD_STR} \"${arg}\"")
endforeach()

# * 执行构建
# * 配置缓存失效会删除整个私有安装前缀，该目录必须只含可再生构建产物。
# * 删除后配置失败不会回滚旧安装内容；本轮注释审计不执行此源码模式命令。
ExternalProject_Add(
  ffmpeg_project
  SOURCE_DIR ${FFMPEG_SOURCE_DIR}
  # * 用占位符保护的 Windows PATH 在 ExternalProject 中恢复列表分隔符。
  LIST_SEPARATOR "${ICE_FFMPEG_LIST_SEPARATOR}"
  UPDATE_COMMAND ""
  # * 每轮进入自定义缓存检查，不等于无条件重新编译所有第三方源码。
  BUILD_ALWAYS TRUE
  # * 前置依赖必须先安装，configure 才能使用真实头文件、元数据和探针链接库。
  DEPENDS zlib_project lame_project
  CONFIGURE_COMMAND
    sh -c
    "${FFMPEG_SOURCE_READY_TEST} || (rm -rf '${FFMPEG_INSTALL_DIR}' && ${FFMPEG_CONFIGURE_CMD_STR})"
    # 统一使用 make，Windows MSVC 环境下 FFmpeg 也通常需要适配好的 make (如 Git Bash 里的)
  # * 外部步骤仍使用 make，即使顶层生成器为 Ninja 也需宿主提供兼容 make。
  # * PROCESSOR_COUNT 来自调用环境，本文件不探测或限制它，不能假定等于外层 -j。
  BUILD_COMMAND
    ${CMAKE_COMMAND} -E env ${ICE_FFMPEG_ENV} "PATH=${ICE_FFMPEG_TOOL_PATH}" sh
    -c "${FFMPEG_SOURCE_READY_TEST} || make -j${PROCESSOR_COUNT}"
    # 执行安装，成功后立刻删除 share 目录，保持 install 目录纯净
  # * 安装、清理 share 派生目录和更新戳以成功链串联，任一步失败都不应产生新成功戳。
  # * share 位于私有安装树；清理它不涉及源码资源，但会移除该安装目录的非库附属文件。
  INSTALL_COMMAND
    ${CMAKE_COMMAND} -E env ${ICE_FFMPEG_ENV} "PATH=${ICE_FFMPEG_TOOL_PATH}" sh
    -c
    "${FFMPEG_SOURCE_READY_TEST} || (make install && '${CMAKE_COMMAND}' -E rm -rf '${FFMPEG_INSTALL_DIR}/share' && '${CMAKE_COMMAND}' -E touch '${FFMPEG_CONFIG_STAMP}')"
  BUILD_BYPRODUCTS ${FFMPEG_BYPRODUCTS} ${FFMPEG_CONFIG_STAMP})

# * 确保头文件目录存在，防止 CMake 配置阶段报错
# * 配置期空目录仅满足路径要求，真实公共头仍需等到外部项目安装完成。
file(MAKE_DIRECTORY ${FFMPEG_INCLUDE_DIR})

# --- 封装接口库 ---
set(FFMPEG_LIBS ${FFMPEG_BYPRODUCTS}) # 直接利用上面定义的产物列表
# * 静态 FFmpeg 不封装依赖 DLL，最终消费者还需链接明确的 LAME 与 zlib 归档。
set(FFMPEG_EXTERNAL_LIBRARIES ${ICE_LAME_STATIC_LIBRARY}
                              ${ICE_ZLIB_STATIC_LIBRARY})

# * 系统链接项依据目标平台选择，与前面决定 shell 的宿主平台条件不同。
# * 套接字、COM、安全库属于静态链接集合，不自动扩大 configure 的协议或设备功能。
if(WIN32)
  set(FFMPEG_PLATFORM_LIBRARIES
      bcrypt
      user32
      # COM 与媒体 GUID 解析项属于 Windows 静态消费闭包，不能仅按音频用途裁掉。
      ole32
      strmiids
      uuid
      ws2_32
      secur32
      advapi32
      shell32
      vfw32)
  # * Apple 音视频框架来自目标 SDK，不在项目内创建或复制框架实现。
  # * 压缩与字符转换系统库仍需最终消费者解析，不能只链接 FFmpeg 五个归档。
elseif(APPLE)
  set(FFMPEG_PLATFORM_LIBRARIES
      "-framework CoreFoundation"
      "-framework CoreVideo"
      "-framework CoreMedia"
      "-framework AudioToolbox"
      "-framework VideoToolbox"
      "-framework Security"
      bz2
      # 系统数学和字符转换项保留链接名称，实际查找交由目标 SDK 与链接器完成。
      m
      iconv)
else()
  # * 其他系统统一采用这组 Unix 库名，新增目标平台必须验证，不能视作通用兼容保证。
  set(FFMPEG_PLATFORM_LIBRARIES m pthread lzma bz2 dl)
endif()

# * 包装名与预编译模式一致，业务不必感知 ExternalProject 或私有安装位置。
# * 构建依赖确保外部产物就绪，不对产物符号或实际媒体能力作运行时验证。
add_library(3rd_ffmpeg INTERFACE)
add_dependencies(3rd_ffmpeg ffmpeg_project)
target_include_directories(3rd_ffmpeg INTERFACE ${FFMPEG_INCLUDE_DIR})
# * 按 FFmpeg 库、外部归档、平台库的顺序传播链接闭包，不修改或重新合并归档内容。
target_link_libraries(
  3rd_ffmpeg INTERFACE ${FFMPEG_LIBS} ${FFMPEG_EXTERNAL_LIBRARIES}
                       ${FFMPEG_PLATFORM_LIBRARIES})
