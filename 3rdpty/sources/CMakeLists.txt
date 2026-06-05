# 3rdpty/sources/CMakeLists.txt

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(arch-probe)
if(NOT MSVC)
    include(Buildzlib)
    include(Buildlame)
endif()
include(Buildffmpeg)
include(Buildfftw)
include(Buildlibsamplerate)
include(Buildrubberband)
include(Buildfmt)
include(Buildspdlog)
include(Buildopenal)
include(Buildsdl)
