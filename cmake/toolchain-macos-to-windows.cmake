# macOS 到 Windows x86_64 的历史 MinGW-w64 交叉编译配置。 必须由配置入口显式选择；不负责下载编译器、安装目标依赖或执行
# Windows 程序。

# 声明目标系统及架构，不从当前宿主 CPU 推导，也不包含 ARM 目标分支。
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

# 编译器名称依赖宿主 PATH 解析；前缀相同不保证工具版本与目标运行库兼容。 GCC、G++ 和资源编译器必须能在宿主执行，本文件不校验三者来自同一发行包。
set(TOOLCHAIN_PREFIX x86_64-w64-mingw32)
set(CMAKE_C_COMPILER ${TOOLCHAIN_PREFIX}-gcc)
set(CMAKE_CXX_COMPILER ${TOOLCHAIN_PREFIX}-g++)
set(CMAKE_RC_COMPILER ${TOOLCHAIN_PREFIX}-windres) # 用于处理 Windows 资源文件 (.rc)

# 系统根仍是特定开发机的绝对路径，迁移环境前必须准备相应目录并调整配置。 普通 set 会覆盖当前作用域同名值；本脚本没有提供可移植的路径缓存选项。
# 系统根只描述目标文件布局，不证明库的架构、编译器 ABI 或调试配置匹配。
set(CMAKE_SYSROOT "/Users/xiang2333/Documents/win-mingw64-toolchain/mingw64")

# 为标准查找命令设置目标根；这不是文件访问沙箱，不能约束显式绝对路径引用。
set(CMAKE_FIND_ROOT_PATH ${CMAKE_SYSROOT})

# 构建期间运行的工具在宿主侧查找，避免误选系统根中的 Windows 可执行文件。 此设置不为目标程序提供模拟器，也不使交叉编译后的测试自动可运行。
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)

# 库、头文件和配置模式的包查找默认限定到目标根，减少误选宿主二进制的机会。 Find 模块本身仍从 CMAKE_MODULE_PATH
# 等模块位置加载，并非只能放在系统根。 查找调用可显式覆盖根路径模式，模块也可直接构造路径，不能宣称绝无宿主依赖泄漏。
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# 输出的是本次选择值，不表示目录存在、编译器已通过检测或交叉链接已成功。
message(STATUS "Toolchain: Loaded macOS to Windows MinGW-w64.")
message(STATUS "  - Sysroot: ${CMAKE_SYSROOT}")
message(STATUS "  - C++ Compiler: ${CMAKE_CXX_COMPILER}")
