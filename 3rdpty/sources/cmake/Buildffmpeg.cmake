# 3rdpty/sources/cmake/Buildffmpeg.txt
if(MSVC)
	# 手动把正确的 ffmpeg share 路径加入
	list(APPEND CMAKE_MODULE_PATH
		"${VCPKG_ROOT}/installed/x64-windows-static/share/ffmpeg"
	)
	find_package(FFMPEG REQUIRED)
	add_library(3rd_ffmpeg INTERFACE)
	add_dependencies(3rd_ffmpeg ffmpeg_project)
	target_include_directories(3rd_ffmpeg INTERFACE ${FFMPEG_INCLUDE_DIRS})
	target_link_libraries(3rd_ffmpeg
		INTERFACE ${FFMPEG_LIBRARIES}
	)
	target_link_directories(3rd_ffmpeg
		INTERFACE ${FFMPEG_LIBRARY_DIRS}
	)
else()
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
	set(ICE_FFMPEG_LDFLAGS "")
	set(ICE_FFMPEG_PKG_CONFIG_PATH "${ICE_ZLIB_PKGCONFIG_DIR}")

	if(MSVC)
	else()
		set(ICE_FFMPEG_CFLAGS
			"-I${ICE_ZLIB_INCLUDE_DIR} -I${ICE_LAME_INCLUDE_DIR} -fPIC"
		)
		set(ICE_FFMPEG_LDFLAGS
			"-L${ICE_ZLIB_LIBRARY_DIR} -L${ICE_LAME_LIBRARY_DIR} -fPIC"
		)
	endif()

	# 音频白名单覆盖当前播放器可解码类型，并补齐可用的原生编码器。
	set(ICE_FFMPEG_AUDIO_DECODERS
		flac
		mp3
		aac
		vorbis
		opus
		pcm_s16le
		pcm_s24le
		pcm_f32le
	)
	set(ICE_FFMPEG_AUDIO_ENCODERS
		flac
		aac
		vorbis
		opus
		pcm_s16le
		pcm_s24le
		pcm_f32le
	)

	list(APPEND ICE_FFMPEG_AUDIO_ENCODERS libmp3lame)

	# 常见 BG 视频格式预留：优先编译原生解码器；编码器只启用无外部依赖项。
	set(ICE_FFMPEG_VIDEO_DECODERS
		h264
		hevc
		mpeg4
		mpeg2video
		vp8
		vp9
		av1
		mjpeg
		png
	)
	set(ICE_FFMPEG_VIDEO_ENCODERS
		mpeg4
		mjpeg
		png
	)

	set(ICE_FFMPEG_DEMUXERS
		aac
		avi
		flac
		flv
		h264
		hevc
		ivf
		m4v
		matroska
		mov
		mp3
		mpeg
		mpegts
		ogg
		wav
	)
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
		mpeg
		mpegts
		ogg
		wav
		webm
	)
	set(ICE_FFMPEG_PARSERS
		aac
		aac_latm
		av1
		flac
		h264
		hevc
		mjpeg
		mpeg4video
		mpegaudio
		opus
		png
		vorbis
		vp8
		vp9
	)
	set(ICE_FFMPEG_BSFS
		aac_adtstoasc
		av1_frame_merge
		av1_frame_split
		h264_mp4toannexb
		hevc_mp4toannexb
		vp9_superframe
		vp9_superframe_split
	)

	string(JOIN "," ICE_FFMPEG_DECODER_LIST
		${ICE_FFMPEG_AUDIO_DECODERS}
		${ICE_FFMPEG_VIDEO_DECODERS}
	)
	string(JOIN "," ICE_FFMPEG_ENCODER_LIST
		${ICE_FFMPEG_AUDIO_ENCODERS}
		${ICE_FFMPEG_VIDEO_ENCODERS}
	)
	string(JOIN "," ICE_FFMPEG_DEMUXER_LIST ${ICE_FFMPEG_DEMUXERS})
	string(JOIN "," ICE_FFMPEG_MUXER_LIST ${ICE_FFMPEG_MUXERS})
	string(JOIN "," ICE_FFMPEG_PARSER_LIST ${ICE_FFMPEG_PARSERS})
	string(JOIN "," ICE_FFMPEG_BSF_LIST ${ICE_FFMPEG_BSFS})

	# 定义 FFmpeg 编译参数 (使用 LIST 格式，避免空格引起的引号问题)
	set(FFMPEG_CONF_LIST
		--prefix=${FFMPEG_INSTALL_DIR}
		--arch=${FFMPEG_ARCH} # 自动探测的结果
		--target-os=${FFMPEG_TARGET_OS} # 自动探测的结果
		# --- 指定与顶层项目相同的编译器和工具链 ---
		--cc=${CMAKE_C_COMPILER}
		--cxx=${CMAKE_CXX_COMPILER}
		--ar=${CMAKE_AR}
		--nm=${CMAKE_NM}
		--ranlib=${CMAKE_RANLIB}
		# -----------------------------------
		--disable-all
		--disable-autodetect
		--enable-avcodec
		--enable-avformat
		--enable-swresample
		--enable-swscale
		--enable-avutil
		--enable-zlib
		--disable-programs
		--disable-doc
		--enable-static
		--disable-shared
		--enable-pic # FFmpeg 内部会自动处理一部分 PIC 逻辑
		--pkg-config-flags=--static
		--enable-decoder=${ICE_FFMPEG_DECODER_LIST}
		--enable-encoder=${ICE_FFMPEG_ENCODER_LIST}
		--enable-demuxer=${ICE_FFMPEG_DEMUXER_LIST}
		--enable-muxer=${ICE_FFMPEG_MUXER_LIST}
		--enable-parser=${ICE_FFMPEG_PARSER_LIST}
		--enable-bsf=${ICE_FFMPEG_BSF_LIST}
		--enable-protocol=file
		"--extra-cflags=${ICE_FFMPEG_CFLAGS}"
		"--extra-cxxflags=${ICE_FFMPEG_CFLAGS}"
		"--extra-ldflags=${ICE_FFMPEG_LDFLAGS}" # LDFLAGS 也需要 PIC
		"--extra-libs=-lmp3lame -lz"
	)
	list(APPEND FFMPEG_CONF_LIST --enable-libmp3lame)

	# 调试标志 (直接 append 到 list)
	if(CMAKE_BUILD_TYPE
		STREQUAL
		"Debug"
		OR
		CMAKE_BUILD_TYPE
		STREQUAL
		"RelWithDebInfo"
	)
		list(APPEND FFMPEG_CONF_LIST "--enable-debug" "--disable-stripping")
	else()
		list(APPEND FFMPEG_CONF_LIST "--disable-debug" "--enable-stripping")
	endif()

	# 构造最终命令 (不要带引号展开变量)
	if(WIN32)
		set(FFMPEG_CONFIGURE_CMD
			${CMAKE_COMMAND}
			-E
			env
			"PKG_CONFIG_PATH=${ICE_FFMPEG_PKG_CONFIG_PATH}"
			"PKG_CONFIG_LIBDIR=${ICE_FFMPEG_PKG_CONFIG_PATH}"
			sh.exe
			${FFMPEG_SOURCE_DIR}/configure
			${FFMPEG_CONF_LIST}
		)
	else()
		set(FFMPEG_CONFIGURE_CMD
			${CMAKE_COMMAND}
			-E
			env
			"PKG_CONFIG_PATH=${ICE_FFMPEG_PKG_CONFIG_PATH}"
			"PKG_CONFIG_LIBDIR=${ICE_FFMPEG_PKG_CONFIG_PATH}"
			${FFMPEG_SOURCE_DIR}/configure
			${FFMPEG_CONF_LIST}
		)
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
		${FFMPEG_LIB_DIR}/${LIB_PREFIX}swscale${LIB_EXT}
		${FFMPEG_LIB_DIR}/${LIB_PREFIX}swresample${LIB_EXT}
		${FFMPEG_LIB_DIR}/${LIB_PREFIX}avutil${LIB_EXT}
	)

	string(SHA256 ICE_FFMPEG_CONFIG_HASH "${FFMPEG_CONF_LIST}")
	set(FFMPEG_CONFIG_STAMP
		"${FFMPEG_INSTALL_DIR}/.mmm_ffmpeg_${ICE_FFMPEG_CONFIG_HASH}.stamp"
	)
	set(FFMPEG_READY_TEST "test -f '${FFMPEG_CONFIG_STAMP}'")
	foreach(byproduct IN LISTS FFMPEG_BYPRODUCTS)
		set(FFMPEG_READY_TEST
			"${FFMPEG_READY_TEST} && test -f '${byproduct}'"
		)
	endforeach()

	message(STATUS "Debug: FFmpeg Configure Command is")
	message(STATUS "${FFMPEG_CONFIGURE_CMD}")

	# 转换为字符串以便在 sh -c 中使用
	set(FFMPEG_CONFIGURE_CMD_STR "")
	foreach(arg IN LISTS FFMPEG_CONFIGURE_CMD)
		set(FFMPEG_CONFIGURE_CMD_STR "${FFMPEG_CONFIGURE_CMD_STR} \"${arg}\"")
	endforeach()

	# 执行构建
	ExternalProject_Add(
		ffmpeg_project
		SOURCE_DIR
		${FFMPEG_SOURCE_DIR}
		UPDATE_COMMAND ""
		DEPENDS
		zlib_project
		lame_project
		CONFIGURE_COMMAND
		sh -c "${FFMPEG_READY_TEST} || (rm -rf '${FFMPEG_INSTALL_DIR}' && ${FFMPEG_CONFIGURE_CMD_STR})"
		# 统一使用 make，Windows MSVC 环境下 FFmpeg 也通常需要适配好的 make (如 Git Bash 里的)
		BUILD_COMMAND
		sh -c "${FFMPEG_READY_TEST} || make -j${PROCESSOR_COUNT}"
		# 执行安装，成功后立刻删除 share 目录，保持 install 目录纯净
		INSTALL_COMMAND
		sh
		-c
		"${FFMPEG_READY_TEST} || (make install && ${CMAKE_COMMAND} -E rm -rf '${FFMPEG_INSTALL_DIR}/share' && ${CMAKE_COMMAND} -E touch '${FFMPEG_CONFIG_STAMP}')"
		BUILD_BYPRODUCTS
		${FFMPEG_BYPRODUCTS}
		${FFMPEG_CONFIG_STAMP}
	)

	# 确保头文件目录存在，防止 CMake 配置阶段报错
	file(MAKE_DIRECTORY ${FFMPEG_INCLUDE_DIR})

	# --- 封装接口库 ---
	set(FFMPEG_LIBS ${FFMPEG_BYPRODUCTS}) # 直接利用上面定义的产物列表
	set(FFMPEG_EXTERNAL_LIBRARIES
		${ICE_LAME_STATIC_LIBRARY}
		${ICE_ZLIB_STATIC_LIBRARY}
	)

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
			vfw32
		)
	elseif(APPLE)
		set(FFMPEG_PLATFORM_LIBRARIES
			"-framework CoreFoundation"
			"-framework CoreVideo"
			"-framework CoreMedia"
			"-framework AudioToolbox"
			"-framework VideoToolbox"
			"-framework Security"
			bz2
			m
			iconv
		)
	else()
		set(FFMPEG_PLATFORM_LIBRARIES m pthread lzma bz2 dl)
	endif()

	add_library(3rd_ffmpeg INTERFACE)
	add_dependencies(3rd_ffmpeg ffmpeg_project)
	target_include_directories(3rd_ffmpeg INTERFACE ${FFMPEG_INCLUDE_DIR})
	target_link_libraries(3rd_ffmpeg
		INTERFACE
			${FFMPEG_LIBS}
			${FFMPEG_EXTERNAL_LIBRARIES}
			${FFMPEG_PLATFORM_LIBRARIES}
	)
endif()
