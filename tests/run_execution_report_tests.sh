#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 2 ]]; then
  echo "usage: run_execution_report_tests.sh <execution-report-runner> <source-root>" >&2
  exit 2
fi

runner="$1"
source_root="$2"
fixture_root="$source_root/tests/fixtures/execution-report"

if [[ ! -x "$runner" ]]; then
  echo "execution report runner is not executable" >&2
  exit 2
fi
if [[ ! -d "$fixture_root" ]]; then
  echo "execution report fixtures are missing" >&2
  exit 2
fi

"$runner" "$fixture_root"
