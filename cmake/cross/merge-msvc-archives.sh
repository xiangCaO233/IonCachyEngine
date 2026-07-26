#!/usr/bin/env bash
set -euo pipefail

llvmAr="${ICE_LLVM_AR:-llvm-ar-22}"
llvmLib="${ICE_LLVM_LIB:-llvm-lib-22}"

if (( $# < 3 )); then
    printf "error: merge-msvc-archives requires an output and at least two inputs\n" >&2
    exit 1
fi

outputLibrary="$1"
shift
temporaryDirectory="$(mktemp -d)"
trap 'rm -rf -- "${temporaryDirectory}"' EXIT

# LAME 将 SIMD 实现作为独立对象参与 libtool 链接；逐个展开主归档并
# 收集额外对象，可避免嵌套归档，并确保索引由 llvm-lib 按 MSVC COFF 规则重建。
objectFiles=()
archiveIndex=0
for inputFile in "$@"; do
    case "${inputFile}" in
        *.obj | *.o)
            objectFiles+=("${inputFile}")
            continue
            ;;
    esac

    archiveDirectory="${temporaryDirectory}/archive-${archiveIndex}"
    mkdir -p -- "${archiveDirectory}"
    (
        cd "${archiveDirectory}"
        "${llvmAr}" x "${inputFile}"
    )

    while IFS= read -r -d '' objectFile; do
        objectFiles+=("${objectFile}")
    done < <(find "${archiveDirectory}" -maxdepth 1 -type f \( -name '*.obj' -o -name '*.o' \) -print0)
    archiveIndex=$((archiveIndex + 1))
done

if (( ${#objectFiles[@]} == 0 )); then
    printf "error: no object files found in inputs\n" >&2
    exit 1
fi

mkdir -p -- "$(dirname "${outputLibrary}")"
rm -f -- "${outputLibrary}"
"${llvmLib}" /nologo "/OUT:${outputLibrary}" "${objectFiles[@]}"
