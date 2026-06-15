# 3rdpty/sources/cmake/Buildrubberband.cmake

include(ExternalProject)

# 自动映射 CMake 到 Meson 的构建类型
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
	set(RB_BUILD_TYPE "debug")
	set(RB_FLAGS "-g")
else()
	set(RB_BUILD_TYPE "release")
	set(RB_FLAGS "-O3")
endif()

message(STATUS "Rubberband Build Type: ${RB_BUILD_TYPE}")

# Determine LTO flags for Clang/GCC
set(RB_LTO_FLAGS "")
if(NOT APPLE AND CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND (CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo" OR CMAKE_BUILD_TYPE STREQUAL "MinSizeRel"))
	list(APPEND RB_LTO_FLAGS "-flto=thin" "-fsplit-lto-unit")
endif()

# Determine PGO flags
set(RB_PGO_FLAGS "")
if(MMM_PGO_INSTRUMENT)
	if(WIN32)
		set(DEFAULT_PGO_PATH "NUL")
	else()
		set(DEFAULT_PGO_PATH "/dev/null")
	endif()
	set(RB_PGO_FLAGS "-fprofile-instr-generate=${DEFAULT_PGO_PATH}")
elseif(MMM_PGO_USE)
	get_filename_component(ABS_PGO_DATA "${MMM_PGO_DATA}" ABSOLUTE)
	set(RB_PGO_FLAGS "-fprofile-instr-use=${ABS_PGO_DATA}")
endif()

# Combine extra compile and link flags
set(RB_EXTRA_FLAGS_LIST "")
if(RB_LTO_FLAGS)
	list(APPEND RB_EXTRA_FLAGS_LIST ${RB_LTO_FLAGS})
endif()
if(NOT "${RB_PGO_FLAGS}" STREQUAL "")
	list(APPEND RB_EXTRA_FLAGS_LIST "${RB_PGO_FLAGS}")
endif()

string(REPLACE ";" " " RB_EXTRA_FLAGS "${RB_EXTRA_FLAGS_LIST}")

set(C_ARGS_VAL "${RB_FLAGS}")
set(CPP_ARGS_VAL "${RB_FLAGS}")
set(C_LINK_ARGS_VAL "")
set(CPP_LINK_ARGS_VAL "")

if(NOT "${RB_EXTRA_FLAGS}" STREQUAL "")
	set(C_ARGS_VAL "${C_ARGS_VAL} ${RB_EXTRA_FLAGS}")
	set(CPP_ARGS_VAL "${CPP_ARGS_VAL} ${RB_EXTRA_FLAGS}")
	set(C_LINK_ARGS_VAL "${RB_EXTRA_FLAGS}")
	set(CPP_LINK_ARGS_VAL "${RB_EXTRA_FLAGS}")
	message(STATUS "Rubberband Extra Build Flags (LTO/PGO): ${RB_EXTRA_FLAGS}")
endif()

# 定义项目路径配置
get_filename_component(RUBBERBAND_SOURCE_DIR
	"${CMAKE_CURRENT_SOURCE_DIR}/../sources/rubberband"
	ABSOLUTE
)
set(RUBBERBAND_BUILD_DIR "${CMAKE_BINARY_DIR}/rb_bld")
set(RUBBERBAND_INSTALL_DIR "${CMAKE_BINARY_DIR}/rb_inst")

# 本地依赖路径 (FFTW3 和 libsamplerate)
# FFTW3 来自 ExternalProject_Add (fftw_project)
set(FFTW_INST_DIR "${CMAKE_BINARY_DIR}/3rdpty/fftw_inst")
set(LOCAL_FFTW3_INCLUDE "${FFTW_INST_DIR}/include")
set(LOCAL_FFTW3_LIB_DIR "${FFTW_INST_DIR}/lib")

# libsamplerate 来自 Buildlibsamplerate.cmake (samplerate 目标)
set(SAMPLERATE_SRC_DIR "${CMAKE_CURRENT_LIST_DIR}/../libsamplerate")
set(SAMPLERATE_BIN_DIR "${CMAKE_BINARY_DIR}/3rdpty/libsamplerate")
set(LOCAL_SAMPLERATE_INCLUDE "${SAMPLERATE_SRC_DIR}/src;${SAMPLERATE_BIN_DIR}")
set(LOCAL_SAMPLERATE_LIB_DIR "${CMAKE_BINARY_DIR}/3rdpty/libsamplerate")

set(EXTRA_INC_LIST "${LOCAL_FFTW3_INCLUDE}" "${SAMPLERATE_SRC_DIR}/src" "${SAMPLERATE_BIN_DIR}")
set(EXTRA_LIB_LIST "${LOCAL_FFTW3_LIB_DIR}" "${LOCAL_SAMPLERATE_LIB_DIR}")

# Convert lists to comma-separated strings for Meson array options
string(REPLACE ";" "," EXTRA_INC_STR "${EXTRA_INC_LIST}")
string(REPLACE ";" "," EXTRA_LIB_STR "${EXTRA_LIB_LIST}")

# Provide pkg-config files for locally built dependencies so Meson does not
# accidentally discover Homebrew/system copies with incompatible linkage.
set(RUBBERBAND_PKG_CONFIG_DIR "${CMAKE_BINARY_DIR}/3rdpty/rubberband_pkgconfig")
file(MAKE_DIRECTORY "${RUBBERBAND_PKG_CONFIG_DIR}")
file(WRITE "${RUBBERBAND_PKG_CONFIG_DIR}/fftw3.pc"
	"prefix=${FFTW_INST_DIR}\n"
	"exec_prefix=${FFTW_INST_DIR}\n"
	"libdir=${LOCAL_FFTW3_LIB_DIR}\n"
	"includedir=${LOCAL_FFTW3_INCLUDE}\n"
	"\n"
	"Name: FFTW\n"
	"Description: fast Fourier transform library\n"
	"Version: 3.3.10\n"
	"Libs: -L${LOCAL_FFTW3_LIB_DIR} -lfftw3\n"
	"Libs.private: -lm\n"
	"Cflags: -I${LOCAL_FFTW3_INCLUDE}\n"
)
file(WRITE "${RUBBERBAND_PKG_CONFIG_DIR}/samplerate.pc"
	"prefix=${SAMPLERATE_BIN_DIR}\n"
	"exec_prefix=${SAMPLERATE_BIN_DIR}\n"
	"libdir=${LOCAL_SAMPLERATE_LIB_DIR}\n"
	"includedir=${SAMPLERATE_SRC_DIR}/src\n"
	"configincludedir=${SAMPLERATE_BIN_DIR}\n"
	"\n"
	"Name: libsamplerate\n"
	"Description: Sample Rate Converter for audio\n"
	"Version: 0.1.9\n"
	"Libs: -L${LOCAL_SAMPLERATE_LIB_DIR} -lsamplerate\n"
	"Cflags: -I${SAMPLERATE_SRC_DIR}/src -I${SAMPLERATE_BIN_DIR}\n"
)

if(MSVC)
	# 根据主项目的构建类型决定 Meson 的参数
	if(CMAKE_BUILD_TYPE STREQUAL "Debug")
		set(RUBBERBAND_BUILD_TYPE "debug")
		set(RUBBERBAND_CRT "mtd") # 对应 MSVC 的 /MTd
	else()
		set(RUBBERBAND_BUILD_TYPE "release")
		set(RUBBERBAND_CRT "mt") # 对应 MSVC 的 /MT
	endif()

	# 找到 pkg-config
	find_program(PKG_CONFIG_EXE NAMES pkg-config pkgconf PATHS "C:/msys64/mingw64/bin" "C:/msys64/usr/bin")
	if(NOT PKG_CONFIG_EXE)
		set(PKG_CONFIG_EXE "pkg-config")
	endif()

	# 构造环境变量
	set(MESON_ENV
		${CMAKE_COMMAND}
		-E
		env
		"PKG_CONFIG=${PKG_CONFIG_EXE}"
		"PKG_CONFIG_PATH=" # 清空它，确保不使用系统包
		"CMAKE_PREFIX_PATH="
	)

	# 构造参数
	set(MESON_SETUP_ARGS
		--prefix=${RUBBERBAND_INSTALL_DIR}
		--libdir=lib
		--buildtype=${RB_BUILD_TYPE}
		-Db_vscrt=${RUBBERBAND_CRT}
		-Ddefault_library=static
		-Dtests=disabled
		-Dcmdline=disabled
		-Dvamp=disabled
		-Dladspa=disabled
		-Dlv2=disabled
		-Djni=disabled
        -Dfft=fftw
		-Dresampler=libsamplerate
		"-Dextra_include_dirs=${EXTRA_INC_STR}"
		"-Dextra_lib_dirs=${EXTRA_LIB_STR}"
		"-Dc_args=${C_ARGS_VAL}"
		"-Dcpp_args=${CPP_ARGS_VAL}"
	)

	if(NOT "${C_LINK_ARGS_VAL}" STREQUAL "")
		list(APPEND MESON_SETUP_ARGS "-Dc_link_args=${C_LINK_ARGS_VAL}" "-Dcpp_link_args=${CPP_LINK_ARGS_VAL}")
	endif()

	if(CMAKE_CROSSCOMPILING)
		list(APPEND MESON_SETUP_ARGS "--cross-file=${CMAKE_SOURCE_DIR}/cmake/toolchain/meson-cross-cl.toml")
	endif()

	# 确定库文件产物路径
	set(RUBBERBAND_STATIC_LIB "${RUBBERBAND_INSTALL_DIR}/lib/rubberband-static.lib")

	# 将列表转换为字符串，以便在 sh -c 中使用
	set(MESON_SETUP_ARGS_STR "")
	foreach(arg IN LISTS MESON_SETUP_ARGS)
		set(MESON_SETUP_ARGS_STR "${MESON_SETUP_ARGS_STR} \"${arg}\"")
	endforeach()

	ExternalProject_Add(
		rubberband_project
		SOURCE_DIR "${RUBBERBAND_SOURCE_DIR}"
		BINARY_DIR "${RUBBERBAND_BUILD_DIR}"
		DEPENDS fftw_project samplerate
		UPDATE_COMMAND ""
		CONFIGURE_COMMAND sh -c "test -f '${RUBBERBAND_BUILD_DIR}/build.ninja' || PKG_CONFIG='${PKG_CONFIG_EXE}' PKG_CONFIG_PATH='${RUBBERBAND_PKG_CONFIG_DIR}' PKG_CONFIG_LIBDIR='${RUBBERBAND_PKG_CONFIG_DIR}' CMAKE_PREFIX_PATH='' meson setup ${MESON_SETUP_ARGS_STR} --native-file=NUL '${RUBBERBAND_BUILD_DIR}' '${RUBBERBAND_SOURCE_DIR}'"
		BUILD_COMMAND sh -c "test -f '${RUBBERBAND_STATIC_LIB}' || PKG_CONFIG='${PKG_CONFIG_EXE}' PKG_CONFIG_PATH='${RUBBERBAND_PKG_CONFIG_DIR}' PKG_CONFIG_LIBDIR='${RUBBERBAND_PKG_CONFIG_DIR}' CMAKE_PREFIX_PATH='' meson compile -C '${RUBBERBAND_BUILD_DIR}'"
		INSTALL_COMMAND sh -c "test -f '${RUBBERBAND_STATIC_LIB}' || meson install -C '${RUBBERBAND_BUILD_DIR}' --no-rebuild"
		BUILD_BYPRODUCTS "${RUBBERBAND_STATIC_LIB}"
	)
else()
	# 构造 Meson 配置参数
	set(MESON_SETUP_ARGS
		--prefix=${RUBBERBAND_INSTALL_DIR}
		--libdir=lib
		--buildtype=${RB_BUILD_TYPE}
		-Ddefault_library=static
		-Dtests=disabled
		-Dcmdline=disabled
		-Dvamp=disabled
		-Dladspa=disabled
		-Dlv2=disabled
		-Djni=disabled
		-Dfft=fftw
		-Dresampler=libsamplerate
		"-Dextra_include_dirs=${EXTRA_INC_STR}"
		"-Dextra_lib_dirs=${EXTRA_LIB_STR}"
		"-Dc_args=${C_ARGS_VAL}"
		"-Dcpp_args=${CPP_ARGS_VAL}"
	)

	if(NOT "${C_LINK_ARGS_VAL}" STREQUAL "")
		list(APPEND MESON_SETUP_ARGS "-Dc_link_args=${C_LINK_ARGS_VAL}" "-Dcpp_link_args=${CPP_LINK_ARGS_VAL}")
	endif()
	
	set(RUBBERBAND_STATIC_LIB "${RUBBERBAND_INSTALL_DIR}/lib/librubberband.a")
	
	# 将列表转换为字符串，以便在 sh -c 中使用
	set(MESON_SETUP_ARGS_STR "")
	foreach(arg IN LISTS MESON_SETUP_ARGS)
		set(MESON_SETUP_ARGS_STR "${MESON_SETUP_ARGS_STR} \"${arg}\"")
	endforeach()

	# Meson does not automatically inherit CMake's compiler/toolchain selection.
	set(MESON_TOOLCHAIN_ENV "CC='${CMAKE_C_COMPILER}' CXX='${CMAKE_CXX_COMPILER}' AR='${CMAKE_AR}' RANLIB='${CMAKE_RANLIB}' NM='${CMAKE_NM}' PKG_CONFIG_PATH='${RUBBERBAND_PKG_CONFIG_DIR}' PKG_CONFIG_LIBDIR='${RUBBERBAND_PKG_CONFIG_DIR}'")

	ExternalProject_Add(
		rubberband_project
		SOURCE_DIR "${RUBBERBAND_SOURCE_DIR}"
		BINARY_DIR "${RUBBERBAND_BUILD_DIR}"
		DEPENDS fftw_project samplerate
		UPDATE_COMMAND ""
		CONFIGURE_COMMAND sh -c "test -f '${RUBBERBAND_BUILD_DIR}/build.ninja' || ${MESON_TOOLCHAIN_ENV} meson setup ${MESON_SETUP_ARGS_STR} '${RUBBERBAND_BUILD_DIR}' '${RUBBERBAND_SOURCE_DIR}'"
		BUILD_COMMAND sh -c "test -f '${RUBBERBAND_STATIC_LIB}' || ${MESON_TOOLCHAIN_ENV} meson compile -C '${RUBBERBAND_BUILD_DIR}'"
		INSTALL_COMMAND sh -c "test -f '${RUBBERBAND_STATIC_LIB}' || ${MESON_TOOLCHAIN_ENV} meson install -C '${RUBBERBAND_BUILD_DIR}' --no-rebuild"
		BUILD_BYPRODUCTS "${RUBBERBAND_STATIC_LIB}"
	)
endif()

# 封装为 CMake 接口库
file(MAKE_DIRECTORY "${RUBBERBAND_INSTALL_DIR}/include")

add_library(3rd_rubberband INTERFACE)
add_dependencies(3rd_rubberband rubberband_project)

target_include_directories(3rd_rubberband INTERFACE "${RUBBERBAND_INSTALL_DIR}/include")
target_link_libraries(3rd_rubberband INTERFACE "${RUBBERBAND_STATIC_LIB}")

# 链接依赖的静态库
target_link_libraries(3rd_rubberband INTERFACE 3rd_fftw3 3rd_libsamplerate)

# 系统底层库链接
if(WIN32)
	if(NOT MSVC)
		target_link_libraries(3rd_rubberband INTERFACE pthread)
	endif()
else()
	target_link_libraries(3rd_rubberband INTERFACE m)
	if(NOT APPLE)
		target_link_libraries(3rd_rubberband INTERFACE pthread)
	endif()
endif()
