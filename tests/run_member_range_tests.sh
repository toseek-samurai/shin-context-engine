#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 2 ]]; then
  echo "usage: run_member_range_tests.sh <shin-context> <source-root>" >&2
  exit 2
fi

tool="$1"
source_root="$2"
fixture="$source_root/tests/fixtures/member-ranges"
work_root="$(mktemp -d "${TMPDIR:-/tmp}/shin-member-ranges.XXXXXX")"
trap 'rm -rf "$work_root"' EXIT

root="$work_root/root"
out="$work_root/out"
facts="$out/docs/kb/generated/facts.jsonl"
links="$out/docs/kb/generated/links.jsonl"
cp -R "$fixture/root" "$root"
printf 'export function overCapHeader(): string {\n' >"$root/src/over-cap.ts"
dd if=/dev/zero bs=1048577 count=1 status=none >>"$root/src/over-cap.ts"
printf '\nreturn "cap";\n}\n' >>"$root/src/over-cap.ts"

shin() {
  "$tool" --profile member-ranges --profile-dir "$fixture/profiles" \
    --root "$root" --out-dir "$out" "$@"
}

assert_syntax_fact() {
  local path="$1" record_type="$2" symbol="$3" start="$4" end="$5"
  if ! jq -se --arg path "$path" --arg record_type "$record_type" --arg symbol "$symbol" \
      --argjson start "$start" --argjson end "$end" '
      any(.[]; .path == $path and .record_type == $record_type and .symbol_name == $symbol and
        .start_line == $start and .end_line == $end and .range_origin == "syntax" and
        (.span_hash | test("^fnv1a64:[0-9a-f]{16}$")))' "$facts" >/dev/null; then
    echo "missing syntax fact: $path $record_type $symbol [$start,$end]" >&2
    exit 1
  fi
}

assert_syntax_identifier_scope() {
  local path="$1" record_type="$2" symbol="$3" start="$4" end="$5" identifier="$6"
  if ! jq -se --arg path "$path" --arg record_type "$record_type" --arg symbol "$symbol" \
      --arg identifier "$identifier" --argjson start "$start" --argjson end "$end" '
      any(.[]; .path == $path and .record_type == $record_type and .symbol_name == $symbol and
        .start_line == $start and .end_line == $end and .range_origin == "syntax" and
        (.identifier_terms | index($identifier) != null))' "$facts" >/dev/null; then
    echo "missing exact identifier scope: $path $record_type $symbol $identifier" >&2
    exit 1
  fi
}

assert_syntax_header() {
  local path="$1" record_type="$2" symbol="$3" line="$4"
  if ! jq -se --arg path "$path" --arg record_type "$record_type" --arg symbol "$symbol" \
      --argjson line "$line" '
      any(.[]; .path == $path and .record_type == $record_type and .symbol_name == $symbol and
        .line == $line and .start_line == $line and .end_line == $line and
        .range_origin == "syntax-header" and (.span_hash | test("^fnv1a64:[0-9a-f]{16}$")))' \
      "$facts" >/dev/null; then
    echo "missing syntax header: $path $record_type $symbol" >&2
    exit 1
  fi
}

assert_no_syntax_fact() {
  local path="$1" record_type="$2" symbol="$3"
  if jq -se --arg path "$path" --arg record_type "$record_type" --arg symbol "$symbol" \
      'any(.[]; .path == $path and .record_type == $record_type and .symbol_name == $symbol and
        .range_origin == "syntax")' "$facts" >/dev/null; then
    echo "unexpected syntax declaration: $path $record_type $symbol" >&2
    exit 1
  fi
}

assert_lexical_fact() {
  local path="$1" record_type="$2" symbol="$3"
  if ! jq -se --arg path "$path" --arg record_type "$record_type" --arg symbol "$symbol" '
      any(.[]; .path == $path and .record_type == $record_type and .symbol_name == $symbol and
        .range_origin == "lexical" and .start_line <= .line and .line <= .end_line and
        (.span_hash | test("^fnv1a64:[0-9a-f]{16}$")))' "$facts" >/dev/null; then
    echo "missing lexical fallback fact: $path $record_type $symbol" >&2
    exit 1
  fi
}

assert_link() {
  local path="$1" target="$2" status="$3" resolved="$4" line="$5"
  if ! jq -se --arg path "$path" --arg target "$target" --arg status "$status" \
      --arg resolved "$resolved" --argjson line "$line" '
      any(.[]; .path == $path and .kind == "source-import" and .target == $target and
        .status == $status and .resolved_path == $resolved and .line == $line)' "$links" >/dev/null; then
    echo "missing source import: $path:$line $target $status $resolved" >&2
    exit 1
  fi
}

assert_no_link_target() {
  local target="$1"
  if jq -se --arg target "$target" 'any(.[]; .kind == "source-import" and .target == $target)' \
      "$links" >/dev/null; then
    echo "unexpected source import target: $target" >&2
    exit 1
  fi
}

shin profile-check --strict >/dev/null
shin build >/dev/null
shin check >/dev/null

# Java syntax: annotations expand the declaration start; constructors are never methods.
assert_syntax_fact src/MemberRanges.java java-class MemberRanges 8 36
assert_syntax_fact src/MemberRanges.java java-constructor MemberRanges 10 13
assert_syntax_fact src/MemberRanges.java java-method javaMultiline 15 20
assert_syntax_fact src/MemberRanges.java java-method overloaded 22 24
assert_syntax_fact src/MemberRanges.java java-method overloaded 26 28
assert_syntax_identifier_scope src/MemberRanges.java java-method javaMultiline 15 20 retryCount
assert_no_syntax_fact src/MemberRanges.java java-method run
assert_syntax_header src/MemberRanges.java java-package fixture.ranges 1

# TS and TSX use syntax ranges for annotated members, overloads, constructors, and arrows.
assert_syntax_fact src/member-ranges.ts ts-class TypeRange 7 27
assert_syntax_fact src/member-ranges.ts ts-method multilineMember 9 15
assert_syntax_fact src/member-ranges.ts ts-method overloaded 17 17
assert_syntax_fact src/member-ranges.ts ts-method overloaded 18 20
assert_syntax_fact src/member-ranges.ts ts-method constructor 22 26
assert_syntax_fact src/member-ranges.ts ts-type RangeAlias 29 29
assert_syntax_fact src/member-ranges.ts ts-arrow-function multilineArrow 31 35
assert_syntax_identifier_scope src/member-ranges.ts ts-method multilineMember 9 15 retries
assert_syntax_identifier_scope src/member-ranges.ts ts-arrow-function multilineArrow 31 35 value
assert_no_syntax_fact src/member-ranges.ts ts-method run

assert_syntax_fact src/member-ranges.tsx ts-class TsxRange 3 17
assert_syntax_fact src/member-ranges.tsx ts-method render 5 10
assert_syntax_fact src/member-ranges.tsx ts-method constructor 12 16
assert_syntax_fact src/member-ranges.tsx ts-arrow-function TsxArrow 19 21
assert_syntax_identifier_scope src/member-ranges.tsx ts-method render 5 10 value

# Relative literal imports become relations; external and escaped literals do not.
assert_link src/member-ranges.ts ./relative-target resolved src/relative-target.ts 1
assert_link src/member-ranges.ts ./re-export-target resolved src/re-export-target.ts 2
assert_link src/member-ranges.ts ./ambiguous ambiguous '' 3
assert_link src/member-ranges.tsx ./tsx-target resolved src/tsx-target.tsx 1
assert_no_link_target './escaped\path'
assert_no_link_target package-value

# Java binds only actual top-level program declarations and skips wildcard/static imports.
assert_link src/MemberRanges.java fixture.ranges.TopLevelHelper resolved src/TopLevelHelper.java 3
assert_link src/MemberRanges.java fixture.ranges.MemberRanges.Nested unresolved '' 4
assert_no_link_target fixture.ranges
assert_no_link_target fixture.ranges.TopLevelHelper.helper

# Syntax recovery retains only complete declaration nodes and publishes an explicit incomplete row.
assert_syntax_fact src/incomplete-range.ts ts-function validBeforeError 1 5
assert_no_syntax_fact src/incomplete-range.ts ts-function broken
assert_syntax_header src/incomplete-range.ts ts-syntax-incomplete incomplete-syntax 1
assert_lexical_fact src/over-cap.ts ts-function overCapHeader
assert_syntax_header src/over-cap.ts ts-syntax-incomplete incomplete-syntax 1

# C# and Kotlin remain lexical; C++ uses the syntax path.
assert_lexical_fact src/MemberRanges.cs csharp-class CsharpRange
assert_lexical_fact src/MemberRanges.cs csharp-method MultilineMember
assert_lexical_fact src/MemberRanges.kt kotlin-class KotlinRange
assert_lexical_fact src/MemberRanges.kt kotlin-function multilineMember
assert_syntax_fact src/member_ranges.cpp cpp-class CppRange 1 8
assert_syntax_fact src/member_ranges.cpp cpp-function multilineMember 3 7
assert_syntax_fact src/member_ranges.cpp cpp-function oneLine 10 10
assert_syntax_fact src/member_ranges.cpp cpp-function Constructed 15 15
assert_no_syntax_fact src/member_ranges.cpp cpp-struct stat
assert_no_syntax_fact src/member_ranges.cpp cpp-function policy_
jq -se '[.[] | select(.path == "src/member_ranges.cpp" and .symbol_name == "sameLine")] |
  length == 2 and ([.[].id] | unique | length) == 2 and
  all(.[]; .start_line == 11 and .end_line == 11 and .range_origin == "syntax")' "$facts" >/dev/null

# Overload spans must be independently hashed even with one shared symbol name.
jq -se '
  [ .[] | select(.path == "src/MemberRanges.java" and .record_type == "java-method" and
    .symbol_name == "overloaded" and .range_origin == "syntax") ] |
  length == 2 and ([.[].span_hash] | unique | length) == 2' "$facts" >/dev/null

# A second build over the same generated cap source must remain byte-identical.
second="$work_root/second"
"$tool" --profile member-ranges --profile-dir "$fixture/profiles" \
  --root "$root" --out-dir "$second" build >/dev/null
diff -r "$out/docs/kb/generated" "$second/docs/kb/generated"

# Source relation inputs fail closed before publication when a target field exceeds its bound.
overflow_source="$root/src/import-overflow.ts"
overflow_target="./"
while [[ ${#overflow_target} -le 4096 ]]; do overflow_target+="a"; done
printf 'import overflow from "%s";\n' "$overflow_target" >"$overflow_source"
overflow_out="$work_root/overflow-out"
overflow_result="$work_root/overflow-result.json"
if "$tool" --profile member-ranges --profile-dir "$fixture/profiles" \
    --root "$root" --out-dir "$overflow_out" build >"$overflow_result" 2>&1; then
  echo "source import field bound unexpectedly published" >&2
  exit 1
fi
if ! grep -Fq 'source-relations-limit' "$overflow_result"; then
  echo "source import field bound returned the wrong error" >&2
  exit 1
fi
if [[ -e "$overflow_out/docs/kb/generated/manifest.json" ]]; then
  echo "source import field bound published a manifest" >&2
  exit 1
fi

echo "member range tests passed"
