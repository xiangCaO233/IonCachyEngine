#!/usr/bin/env bash
set -euo pipefail

# 为构建工具提供 ar 风格入口，实际只支持重新生成一个 MSVC COFF 库。
# 不模拟 ar 的查询、删除或增量更新语义，调用方必须传入完整成员列表。
llvmLib="${ICE_LLVM_LIB:-llvm-lib-22}"

if (( $# < 1 )); then
    # 无输出参数时拒绝执行，避免后续把选项或对象误当成目标文件。
    printf "error: llvm-lib ar compatibility wrapper requires an output library\n" >&2
    exit 1
fi

# GNU ar 的可选首个参数只描述归档操作；llvm-lib 使用 /OUT: 指定同一目标。
if [[ "$1" =~ ^-?[a-zA-Z]+$ ]] && (( $# >= 2 )); then
    # 这里只按字母形状识别操作串，纯字母输出名也会被跳过；路径应带目录或扩展名。
    shift
fi

outputLibrary="$1"
shift
# 重新打包会先删除旧输出；失败不恢复旧库，目标不得与输入对象或归档重合。
rm -f -- "${outputLibrary}"
# 参数数组原样透传，/OUT: 与文件路径组成一个参数，保留路径中的空格。
exec "${llvmLib}" /nologo "/OUT:${outputLibrary}" "$@"
