# fmt 源码构建入口，仅在调用方选择源码依赖时包含，不查找预编译包。

# 路径基于包含此脚本时的源码目录，不能当成独立 cmake -P 脚本运行。
set(FMT_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../sources/fmt")

# 在进入依赖目录前强制关闭文档、测试、模糊测试和安装，限定依赖构建的职责。 FORCE 同时覆盖命令行和旧缓存中的同名设置，不能指望重新配置保留这些开关。
# 这里只管理附加目标，不设置 fmt 的静态/共享选择。
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

# 要求编译器满足已经选择的语言标准；本句不指定标准版本。
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# EXCLUDE_FROM_ALL 排除无依赖的默认构建；被引擎链接的库仍按依赖关系构建。 该选项不是 IDE 分组设置，SYSTEM
# 也不表示忽略依赖的编译错误。
add_subdirectory(${FMT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR}/fmt_build
                 EXCLUDE_FROM_ALL SYSTEM)

# 为可嵌入共享库的静态对象请求 PIC，实际编译参数取决于目标平台和工具链。 此处修改依赖创建的 fmt 实体目标，不创建另一份包装库或改变其链接类型。
set_target_properties(fmt PROPERTIES POSITION_INDEPENDENT_CODE ON)
