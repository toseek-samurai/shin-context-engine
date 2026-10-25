#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 5 ]]; then
  echo "usage: run_clang_tidy.sh <clang-tidy> <run-clang-tidy> <source-root> <build-dir> <jobs>" >&2
  exit 2
fi

clang_tidy="$1"
run_clang_tidy="$2"
source_root="$3"
build_dir="$4"
jobs="$5"

for tool in "$clang_tidy" "$run_clang_tidy"; do
  if ! command -v "$tool" >/dev/null 2>&1; then
    echo "required clang-tidy tool is unavailable: $tool" >&2
    exit 2
  fi
done
if [[ ! -f "$build_dir/compile_commands.json" ]]; then
  echo "compile command database is missing: $build_dir/compile_commands.json" >&2
  exit 2
fi
if [[ ! "$jobs" =~ ^[1-9][0-9]*$ ]]; then
  echo "clang-tidy jobs must be a positive integer: $jobs" >&2
  exit 2
fi

config="$source_root/.clang-tidy"
"$clang_tidy" --verify-config --config-file="$config"
runner_args=(
  -clang-tidy-binary "$clang_tidy"
  -p "$build_dir"
  -j "$jobs"
  -quiet
  -config-file "$config"
  -header-filter="^${source_root}/src/"
)
if "$run_clang_tidy" --help 2>&1 | grep -q -- "-hide-progress"; then
  runner_args+=( -hide-progress )
fi
"$run_clang_tidy" "${runner_args[@]}" "^${source_root}/src/.*\\.cpp$"
