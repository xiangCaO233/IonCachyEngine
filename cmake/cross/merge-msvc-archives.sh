#!/usr/bin/env bash
set -euo pipefail

# 为 libtool 的归档加额外 SIMD 对象场景合并成员，不把输入归档作为嵌套成员塞入输出。
# LLVM 归档工具和库工具须支持相同目标 COFF 格式，调用方可显式覆盖工具路径。
llvmAr="${ICE_LLVM_AR:-llvm-ar-22}"
llvmLib="${ICE_LLVM_LIB:-llvm-lib-22}"

if (( $# < 3 )); then
    # 接口要求输出及至少两个输入，普通单归档创建应使用更简单的包装入口。
    printf "error: merge-msvc-archives requires an output and at least two inputs\n" >&2
    exit 1
fi

outputLibrary="$1"
shift
temporaryDirectory="$(mktemp -d)"
# 独立临时目录只保存本次解包结果，正常退出和失败退出都由 trap 回收。
trap 'rm -rf -- "${temporaryDirectory}"' EXIT

# LAME 将 SIMD 实现作为独立对象参与 libtool 链接；逐个展开主归档并
# 收集额外对象，可避免嵌套归档，并确保索引由 llvm-lib 按 MSVC COFF 规则重建。
objectFiles=()
archiveIndex=0
for inputFile in "$@"; do
    # 输入归档将在子目录内展开，因此归档路径应为绝对路径，避免切目录后失效。
    case "${inputFile}" in
        *.obj | *.o)
            # 裸对象直接加入成员列表，不进行归档探测或拷贝，须保持到最终打包结束。
            objectFiles+=("${inputFile}")
            continue
            ;;
    esac

    archiveDirectory="${temporaryDirectory}/archive-${archiveIndex}"
    # 不同归档隔离展开，避免来自不同包的同名对象在文件系统中互相覆盖。
    mkdir -p -- "${archiveDirectory}"
    (
        # 子 shell 隔离当前目录变化，输出路径和后续裸对象路径仍按原工作目录解析。
        cd "${archiveDirectory}"
        "${llvmAr}" x "${inputFile}"
    )

    while IFS= read -r -d '' objectFile; do
        # 零分隔读取保留文件名空格和特殊字符，不通过空白切分对象路径。
        objectFiles+=("${objectFile}")
    done < <(find "${archiveDirectory}" -maxdepth 1 -type f \( -name '*.obj' -o -name '*.o' \) -print0)
    # 仅收集顶层对象，非对象归档成员和嵌套目录不参与本次合并。
    archiveIndex=$((archiveIndex + 1))
    # 编号仅用于隔离解包目录，不依据归档名推导临时路径或改变最终对象内容。
done

if (( ${#objectFiles[@]} == 0 )); then
    # 空结果不能产生一个看似成功的空库，以免缺失 SIMD 实现在后续链接才暴露。
    printf "error: no object files found in inputs\n" >&2
    exit 1
fi

# 同一归档内部若有同名成员，展开行为由 llvm-ar 决定；本脚本不保证保留重名成员。
mkdir -p -- "$(dirname "${outputLibrary}")"
# 输出会被覆盖，必须与所有输入路径分离；删除后打包失败不会恢复旧产物。
rm -f -- "${outputLibrary}"
# 使用数组传递所有对象，llvm-lib 重新建立 MSVC 索引，不能沿用输入归档的旧索引。
"${llvmLib}" /nologo "/OUT:${outputLibrary}" "${objectFiles[@]}"
