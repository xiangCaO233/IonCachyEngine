# 3rdpty/sources/cmake/Buildfmt.cmake

# 定义源码路径
set(FMT_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../sources/fmt")

# 覆盖 fmt 的内部选项 在调用 add_subdirectory 之前设置这些变量

# 可以强制覆盖 fmt 的默认行为 这比在命令行传递 -D...=OFF 更可靠

# 我们只关心库本身，关闭所有附加目标
set(FMT_DOC
    OFF
    CACHE BOOL "Disable fmt documentation" FORCE)
set(FMT_TEST
    OFF
    CACHE BOOL "Disable fmt tests" FORCE)
set(FMT_FUZZ
    OFF
    CACHE BOOL "Disable fmt fuzzing" FORCE)
set(FMT_INSTALL
    OFF
    CACHE BOOL "Disable fmt install target" FORCE)

# 确保 fmt 和主项目使用相同的 C++ 标准
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 将 fmt 作为子项目包含进来 EXCLUDE_FROM_ALL

# 确保在 Visual Studio 等 IDE 中不会把 fmt 的目标显示在顶层
add_subdirectory(${FMT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR}/fmt_build
                 EXCLUDE_FROM_ALL)

# 为 fmt 目标开启位置无关代码（PIC）

# 这会确保 libfmt.a 是用 -fPIC 选项编译的，从而可以被链接到共享库中

# fmt 库创建的 target 名称就是 "fmt"
set_target_properties(fmt PROPERTIES POSITION_INDEPENDENT_CODE ON)
