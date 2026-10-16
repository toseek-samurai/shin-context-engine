#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
    echo "usage: static_analysis.sh <server-build-dir>" >&2
    exit 2
fi
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
build="$1"
if [[ ! -f "$build/compile_commands.json" ]]; then
    echo "server compilation database is missing" >&2
    exit 2
fi
tidy="${CLANG_TIDY:-}"
if [[ -z "$tidy" ]]; then
    for candidate in clang-tidy-23 clang-tidy /opt/homebrew/opt/llvm/bin/clang-tidy; do
        if command -v "$candidate" >/dev/null 2>&1; then
            tidy="$(command -v "$candidate")"
            break
        fi
    done
fi
if [[ -z "$tidy" ]]; then
    echo "clang-tidy is required" >&2
    exit 2
fi
args=(-p "$build" '--header-filter=^.*/server/(src|include|tests)/')
# Keep the compilation database's toolchain. Injecting Apple libc++ headers over
# a Homebrew LLVM build mixes two standard libraries and breaks include_next.
"$tidy" "${args[@]}" "$root"/server/src/*.cpp "$root"/server/tests/*_test.cpp
