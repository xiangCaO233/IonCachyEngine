#!/usr/bin/env bash
set -euo pipefail

clangCl="${ICE_CLANG_CL:-clang-cl-22}"
clangC="${ICE_CLANG_C:-clang-22}"
targetTriple="${ICE_MSVC_TARGET_TRIPLE:-x86_64-pc-windows-msvc}"

# Autotools 和 FFmpeg 会使用 GNU 风格参数探测编译器；宏探测改用同版本
# clang 驱动，其余目标编译与链接仍由 clang-cl 和 lld-link 完成。
isMacroProbe=0
isCompileOnly=0
for argument in "$@"; do
    case "${argument}" in
        -dM)
            isMacroProbe=1
            ;;
        -c | /c | -E | /E)
            isCompileOnly=1
            ;;
    esac
done

if (( isMacroProbe )); then
    probeArguments=()
    convertSystemInclude=0
    for argument in "$@"; do
        if (( convertSystemInclude )); then
            probeArguments+=("-isystem" "${argument}")
            convertSystemInclude=0
            continue
        fi
        case "${argument}" in
            -imsvc)
                convertSystemInclude=1
                ;;
            /EHsc | /MT | /MTd | /MD | /MDd | /Z7 | /Zi | /Od | /O[0-9a-zA-Z]* | /Ob[0-9]* | /RTC[0-9]* | /nologo | /c)
                ;;
            *)
                probeArguments+=("${argument}")
                ;;
        esac
    done
    exec "${clangC}" --target="${targetTriple}" "${probeArguments[@]}"
fi

if (( !isCompileOnly )); then
    exec "${clangCl}" --target="${targetTriple}" -fuse-ld=lld "$@"
fi

exec "${clangCl}" --target="${targetTriple}" "$@"
