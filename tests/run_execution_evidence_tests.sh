#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 2 ]]; then
  echo "usage: run_execution_evidence_tests.sh <shin-context> <source-root> [unittest arguments]" >&2
  exit 2
fi

tool="$1"
source_root="$2"
shift 2
exec "${PYTHON:-python3}" -B "$source_root/tests/fixtures/execution-evidence/test_execution_evidence.py" \
  "$tool" "$source_root" "$@"
