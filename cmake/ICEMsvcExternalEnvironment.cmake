# 为不继承 CMake 编译规则的外部项目固化 MSVC SDK 搜索环境。
include_guard(GLOBAL)

# * 返回可传给 cmake -E env 的列表，output 是调用方变量名，不是缓存键。
# * 调用方必须将 separator 交给 ExternalProject 的 LIST_SEPARATOR。
# * separator 必须避开 shell 管道符及路径中实际出现的字符。
# * 路径来自工具链参数，不依赖启动构建时的 INCLUDE/LIB 环境。
# * 仅处理交叉 MSVC；其他平台返回空列表，保留原有行为。
# * 支持工具链生成的独立 -imsvc 与 /libpath: 参数形式。
# * 不解析响应文件；若工具链改用响应文件，应同步扩展此契约。
function(ice_msvc_external_environment output separator)
  set(result "")
  if(MSVC AND CMAKE_CROSSCOMPILING)
    # -imsvc 的路径允许包含空格，先按 shell 引号规则解析成独立参数。
    separate_arguments(flags UNIX_COMMAND "${CMAKE_C_FLAGS}")
    # 保留工具链搜索顺序，大小写兼容代理目录必须位于 SDK 原目录之前。
    set(includes "")
    set(next_include OFF)
    foreach(flag IN LISTS flags)
      # 路径参数只消费一次，即使目录名称以选项前缀开头也不重新解释。
      if(next_include)
        list(APPEND includes "${flag}")
        set(next_include OFF)
      elseif(flag STREQUAL "-imsvc")
        set(next_include ON)
      endif()
    endforeach()
    # 编译驱动和直接调用的链接器都认识 LIB，不把 /libpath 当作源文件传递。
    separate_arguments(flags UNIX_COMMAND "${CMAKE_EXE_LINKER_FLAGS}")
    # 仅提取库目录，其他链接标志仍由各外部项目按自己的协议传递。
    set(libraries "")
    foreach(flag IN LISTS flags)
      if(flag MATCHES "^/[Ll][Ii][Bb][Pp][Aa][Tt][Hh]:(.*)$")
        list(APPEND libraries "${CMAKE_MATCH_1}")
      endif()
    endforeach()
    # 占位符避免分号被 CMake 当成新的命令参数，执行外部步骤前才还原。
    list(JOIN includes "${separator}" include_env)
    list(JOIN libraries "${separator}" library_env)
    # 包装器显式使用已选择的工具路径，避免 PATH 变化切换 LLVM 版本。 这些 ICE_* 名称与 cmake/cross
    # 下的包装脚本接口保持一致。
    list(
      APPEND
      result
      "INCLUDE=${include_env}"
      "LIB=${library_env}"
      "ICE_CLANG_CL=${CMAKE_C_COMPILER}"
      "ICE_LLVM_LIB=${CMAKE_AR}"
      "ICE_LLD_LINK=${CMAKE_LINKER}")
  endif()
  # 不写入全局环境，多个构建目录可使用不同的 SDK 和工具链。
  set(${output}
      "${result}"
      PARENT_SCOPE)
endfunction()
