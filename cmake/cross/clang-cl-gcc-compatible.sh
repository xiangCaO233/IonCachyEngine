#!/usr/bin/env bash
set -euo pipefail

# 包装器适配 GNU 风格的配置探测，不改变最终 MSVC ABI 目标。
# 工具路径允许环境覆盖；两个驱动须来自兼容的 Clang 版本，避免探测结果与编译能力不符。
clangCl="${ICE_CLANG_CL:-clang-cl-22}"
clangC="${ICE_CLANG_C:-clang-22}"
targetTriple="${ICE_MSVC_TARGET_TRIPLE:-x86_64-pc-windows-msvc}"
# 该三元组同时传给两种驱动，宏探测不能意外报告宿主架构的预定义值。

# Autotools 和 FFmpeg 会使用 GNU 风格参数探测编译器；宏探测改用同版本
# clang 驱动，其余目标编译与链接仍由 clang-cl 和 lld-link 完成。
isMacroProbe=0
# 分类只检查显式参数，不展开响应文件；响应文件内的 -dM 不会触发专门探测分支。
isCompileOnly=0
for argument in "$@"; do
    case "${argument}" in
        -dM)
            # 宏列表模式使用 clang 的 GNU 驱动，避免 clang-cl 将其按未知选项处理。
            isMacroProbe=1
            ;;
        -c | /c | -E | /E)
            # 预处理同样不应选择链接器；其余未列出的参数保持透传。
            isCompileOnly=1
            ;;
    esac
done

if (( isMacroProbe )); then
    # 数组保留每个原始参数边界，包含空格的 include 路径不能通过字符串重组。
    probeArguments=()
    # 过滤仅作用于宏探测副本，原始 "$@" 保持完整供普通编译与链接使用。
    convertSystemInclude=0
    for argument in "$@"; do
        if (( convertSystemInclude )); then
            # -imsvc 与其后独立路径成对转换，不修改路径自身的编码或分隔符。
            probeArguments+=("-isystem" "${argument}")
            convertSystemInclude=0
            # 已消费的路径不再进入选项匹配，即使路径文本看起来像编译参数。
            continue
        fi
        case "${argument}" in
            -imsvc)
                # 只识别单独参数形式；调用方应提供后续路径，当前脚本不检查缺参。
                convertSystemInclude=1
                ;;
            /EHsc | /MT | /MTd | /MD | /MDd | /Z7 | /Zi | /Od | /O[0-9a-zA-Z]* | /Ob[0-9]* | /RTC[0-9]* | /nologo | /c)
                # 这些 MSVC 编译设置不用于 GNU 驱动宏探测，实际目标编译仍保留原参数。
                ;;
            *)
                # 未识别参数不猜测等价转换，保持探测调用者给定的宏和输入文件。
                probeArguments+=("${argument}")
                ;;
        esac
    done
    # 替换包装进程，直接返回工具退出状态；失败不得伪装成可用特性。
    exec "${clangC}" --target="${targetTriple}" "${probeArguments[@]}"
fi

# 未识别的仅编译形式仍按可链接调用处理，兼容性范围以显式分类项为准。
if (( !isCompileOnly )); then
    # 只有可能链接的调用显式选 lld，避免只编译探测因无用链接选项产生噪声。
    exec "${clangCl}" --target="${targetTriple}" -fuse-ld=lld "$@"
fi

# 普通目标编译保留运行库、异常及调试参数，不使用宏探测分支的过滤结果。
exec "${clangCl}" --target="${targetTriple}" "$@"
