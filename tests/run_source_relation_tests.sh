#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 2 ]]; then
  echo "usage: run_source_relation_tests.sh <shin-context> <source-root>" >&2
  exit 2
fi

tool="$1"
source_root="$2"
fixture="$source_root/tests/fixtures/source-relations"
work_root="$(mktemp -d "${TMPDIR:-/tmp}/shin-source-relations.XXXXXX")"
trap 'rm -rf "$work_root"' EXIT
root="$work_root/root"
out="$work_root/out"
cp -R "$fixture/root" "$root"
links="$out/docs/kb/generated/links.jsonl"
alpha="apps/admin/Alpha"
beta="apps/admin/Beta"

shin() {
  "$tool" --profile source-relations --profile-dir "$fixture/profiles" \
    --root "$root" --out-dir "$out" "$@"
}

assert_link() {
  local path="$1" kind="$2" target="$3" status="$4" resolved="$5" line="$6"
  if ! jq -se --arg path "$path" --arg kind "$kind" --arg target "$target" \
      --arg status "$status" --arg resolved "$resolved" --argjson line "$line" \
      'map(select(.path == $path and .kind == $kind and .target == $target and
                  .status == $status and .resolved_path == $resolved and .line == $line)) | length == 1' \
      "$links" >/dev/null; then
    echo "missing exact link: $path:$line $kind $target $status $resolved" >&2
    exit 1
  fi
}

test_link_schema() {
  jq -e '.schema_version == "shin.context.kb-enterprise.v1" and .profile == "source-relations"' \
    "$out/docs/kb/generated/manifest.json" >/dev/null
  jq -se 'length > 0 and all(.[];
    (.path | type == "string") and (.kind | type == "string") and
    (.text | type == "string") and (.target | type == "string") and
    (.resolved_path | type == "string") and (.line | type == "number" and . >= 1) and
    (.status == "resolved" or .status == "unresolved" or .status == "ambiguous"))' "$links" >/dev/null
}

test_sql_payload_dependencies() {
  assert_link "$alpha/main.rsx" source-include './queries.rsx' resolved "$alpha/queries.rsx" 3
  assert_link "$alpha/main.rsx" source-include './src/View.rsx' resolved "$alpha/src/View.rsx" 5
  assert_link "$alpha/queries.rsx" source-payload './lib/LoadReport.sql' resolved "$alpha/lib/LoadReport.sql" 5
  assert_link "$alpha/lib/PageInit.js" source-reference LoadReport resolved "$alpha/lib/LoadReport.sql" 1
  assert_link "$alpha/lib/LoadReport.sql" source-reference PageInit resolved "$alpha/lib/PageInit.js" 3
  assert_link "$alpha/lib/LoadReport.sql" source-reference UI resolved "$alpha/src/View.rsx" 4
}

test_event_and_inline_lines() {
  assert_link "$alpha/src/View.rsx" source-reference LoadReport resolved "$alpha/lib/LoadReport.sql" 2
  assert_link "$alpha/src/View.rsx" source-reference LoadReport resolved "$alpha/lib/LoadReport.sql" 5
  assert_link "$alpha/src/View.rsx" source-reference PageInit resolved "$alpha/lib/PageInit.js" 10
  assert_link "$alpha/src/View.rsx" source-reference UI resolved "$alpha/src/View.rsx" 11
  assert_link "$alpha/src/View.rsx" source-reference PageInit resolved "$alpha/lib/PageInit.js" 23
  assert_link "$alpha/lib/PageInit.js" source-reference StateValue resolved "$alpha/queries.rsx" 16
  assert_link "$alpha/lib/PageInit.js" source-reference PageInit resolved "$alpha/lib/PageInit.js" 17
}

test_duplicate_missing_and_app_scopes() {
  assert_link "$alpha/lib/PageInit.js" source-reference Duplicate ambiguous '' 7
  assert_link "$alpha/lib/PageInit.js" source-reference Missing unresolved '' 8
  assert_link "$alpha/lib/PageInit.js" source-reference OtherAppOnly unresolved '' 13
  assert_link "$beta/lib/PageInit.js" source-reference PageInit resolved "$beta/lib/PageInit.js" 1
  assert_link "$beta/lib/PageInit.js" source-reference OtherAppOnly resolved "$beta/main.rsx" 2
  assert_link "$beta/main.rsx" source-reference PageInit resolved "$beta/lib/PageInit.js" 4
}

test_same_line_duplicate_declarations() {
  assert_link "$beta/lib/SameLine.js" source-reference SameLineDuplicate ambiguous '' 1
}

test_principal_payload_selection() {
  assert_link "$alpha/queries.rsx" source-payload './lib/headers.json' resolved "$alpha/lib/headers.json" 10
  assert_link "$alpha/queries.rsx" source-payload './lib/body.json' resolved "$alpha/lib/body.json" 11
  assert_link "$alpha/lib/PageInit.js" source-reference Request resolved "$alpha/queries.rsx" 9
  assert_link "$alpha/lib/PageInit.js" source-reference Compound resolved "$alpha/queries.rsx" 11
  assert_link "$alpha/lib/PageInit.js" source-reference BrokenPayload unresolved '' 10
  assert_link "$alpha/lib/PageInit.js" source-reference Included resolved "$alpha/src/View.rsx" 18
  assert_link "$alpha/src/View.rsx" source-payload '../lib/First.sql' resolved "$alpha/lib/First.sql" 25
  assert_link "$alpha/lib/PayloadSelection.js" source-reference RequestQuery resolved "$alpha/queries.rsx" 1
  assert_link "$alpha/lib/PayloadSelection.js" source-reference UnindexedPayload unresolved '' 2
  assert_link "$alpha/lib/PayloadSelection.js" source-reference CommentedInclude resolved "$alpha/lib/First.sql" 3
  assert_link "$alpha/lib/PayloadSelection.js" source-reference Derived resolved "$alpha/lib/First.sql" 4
  assert_link "$alpha/lib/PayloadSelection.js" source-reference CompoundFunction resolved "$alpha/queries.rsx" 5
  assert_link "$alpha/lib/PayloadSelection.js" source-reference WrongFunctionAttribute resolved "$alpha/queries.rsx" 6
  assert_link "$alpha/lib/PayloadSelection.js" source-reference DynamicFunction resolved "$alpha/queries.rsx" 7
}

test_quoted_comments_and_dynamic_values_absent() {
  jq -se 'all(.[] | select(.kind == "source-reference");
    (.target | test("Quoted|CommentIn|Comment$|TemplateInJs|ScriptString|PlainInRsx|Nested|Regex|Computed|chosenTarget") | not))' \
    "$links" >/dev/null
  jq -se 'all(.[] | select(.kind == "source-payload" or .kind == "source-include");
    (.target | test("Phantom|dynamic|comment|^\\./lib/$") | not))' "$links" >/dev/null
  jq -se --arg path "$alpha/lib/LoadReport.sql" \
    'map(select(.path == $path and .kind == "source-reference")) | length == 2' "$links" >/dev/null
  # Legacy textual facts may still mention quoted source; they must not create links.
  jq -se --arg path "$alpha/lib/PageInit.js" \
    'any(.[]; .path == $path and .record_type == "retool-call" and .start_line == 2)' \
    "$out/docs/kb/generated/facts.jsonl" >/dev/null
}

test_unindexed_and_escaping_paths() {
  assert_link "$alpha/main.rsx" source-include './Missing.rsx' unresolved '' 6
  assert_link "$alpha/main.rsx" source-include '../../../../outside.rsx' unresolved '' 7
  assert_link "$alpha/main.rsx" source-include './not-indexed.txt' unresolved '' 8
  assert_link "$alpha/queries.rsx" source-payload '../../../../outside.js' unresolved '' 17
  assert_link "$alpha/lib/PageInit.js" source-reference Traversal unresolved '' 12
  jq -se 'all(.[] | select(.status != "resolved"); .resolved_path == "")' "$links" >/dev/null
}

test_workflow_binding_scope() {
  assert_link workflows/Sample/Read.js source-reference WorkflowStart resolved workflows/Sample/Start.yml 1
  assert_link workflows/Sample/Read.js source-reference PageInit unresolved '' 2
}

test_deterministic_consumer_read_only() {
  "$tool" --profile source-relations --profile-dir "$fixture/profiles" \
    --root "$root" --out-dir "$work_root/second" build >/dev/null
  diff -r "$out/docs/kb/generated" "$work_root/second/docs/kb/generated"
  diff -r "$fixture/root" "$root"
}

test_payload_file_span_hash() {
  local facts="$out/docs/kb/generated/facts.jsonl"
  local path="$alpha/lib/PageInit.js"
  local before
  before="$(jq -sr --arg path "$path" \
    '.[] | select(.path == $path and .record_type == "retool-payload") | .span_hash' "$facts")"
  jq -se --arg path "$path" 'map(select(.path == $path and .record_type == "retool-payload")) |
    length == 1 and .[0].start_line == 1 and .[0].end_line == 18 and
    .[0].symbol_name == "PageInit" and .[0].range_origin == "payload-file" and
    (.[0].span_hash | test("^fnv1a64:[0-9a-f]{16}$"))' \
    "$facts" >/dev/null
  cp "$links" "$work_root/body-links.jsonl"
  sed 's/const literal =/const renamedLiteral =/' "$root/$path" >"$work_root/changed-body.js"
  cp "$work_root/changed-body.js" "$root/$path"
  shin build >/dev/null
  jq -se --arg path "$path" --arg before "$before" \
    'any(.[]; .path == $path and .record_type == "retool-payload" and
      .start_line == 1 and .end_line == 18 and .span_hash != $before and
      (.span_hash | test("^fnv1a64:[0-9a-f]{16}$")))' "$facts" >/dev/null
  cmp "$links" "$work_root/body-links.jsonl"
  cp "$fixture/root/$path" "$root/$path"
}

assert_relation_limit() {
  if shin build >"$work_root/limit.stdout" 2>"$work_root/limit.stderr"; then
    echo 'relation input overflow must fail, not truncate' >&2
    exit 1
  fi
  jq -e '.schema_version == "shin.error.v1" and .error.code == "source-relations-limit"' \
    "$work_root/limit.stderr" >/dev/null
}

test_per_file_relation_limit() {
  awk 'BEGIN { for (i = 0; i < 4096; ++i) print "Missing.trigger();" }' \
    >"$root/$alpha/lib/Limit.js"
  shin build >/dev/null
  jq -se --arg path "$alpha/lib/Limit.js" \
    'map(select(.path == $path and .kind == "source-reference")) | length == 4096' "$links" >/dev/null
  cp "$links" "$work_root/limit-links.jsonl"
  printf 'Missing.trigger();\n' >>"$root/$alpha/lib/Limit.js"
  assert_relation_limit
  cmp "$links" "$work_root/limit-links.jsonl"
  rm "$root/$alpha/lib/Limit.js"

  # 1365 * (binding + payload + reference) + one binding reaches exactly 4096.
  awk 'BEGIN {
    for (i = 0; i < 1365; ++i)
      printf "<JavascriptQuery id=\"Bound%d\" query={include(\"./lib/First.sql\", \"string\")} disabled=\"{{ PageInit.data }}\" />\n", i;
    print "<State id=\"Boundary\" />";
  }' >"$root/$alpha/Limit.rsx"
  shin build >/dev/null
  printf '<State id="Overflow" />\n' >>"$root/$alpha/Limit.rsx"
  assert_relation_limit
  rm "$root/$alpha/Limit.rsx"
  diff -r "$fixture/root" "$root"
}

shin profile-check --strict >/dev/null
shin build >/dev/null
shin check >/dev/null

tests=(
  test_link_schema
  test_sql_payload_dependencies
  test_event_and_inline_lines
  test_duplicate_missing_and_app_scopes
  test_principal_payload_selection
  test_quoted_comments_and_dynamic_values_absent
  test_unindexed_and_escaping_paths
  test_workflow_binding_scope
  test_deterministic_consumer_read_only
  test_payload_file_span_hash
  test_per_file_relation_limit
  test_same_line_duplicate_declarations
)
for test in "${tests[@]}"; do
  "$test"
  echo "passed: $test"
done
echo "source relation tests passed (${#tests[@]})"
