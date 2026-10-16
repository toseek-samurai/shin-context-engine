# Execution report parser fixtures

The runner is intentionally independent of the main build while parent integration adds
the execution-report translation units, pugixml, and the existing simdjson runtime to the Core target.

Compile and run from the repository root:

```sh
c++ -std=c++20 -Wall -Wextra -Wpedantic -Werror -isystem third_party -Isrc \
  src/execution_report.cpp src/execution_report_junit.cpp src/execution_report_playwright.cpp \
  third_party/pugixml/pugixml.cpp third_party/simdjson/simdjson.cpp \
  tests/fixtures/execution-report/runner.cpp -o build/execution-report-runner
build/execution-report-runner tests/fixtures/execution-report
```

Fixture semantics follow primary sources:

- Playwright JSON reporter implementation: https://github.com/microsoft/playwright/blob/main/packages/playwright/src/reporters/json.ts
- Playwright reporter output types and retry outcome implementation: https://github.com/microsoft/playwright/blob/main/packages/playwright/types/testReporter.d.ts and https://github.com/microsoft/playwright/blob/main/packages/playwright/src/isomorphic/teleReceiver.ts
- Playwright reporters guide: https://playwright.dev/docs/test-reporters
- Gradle JUnit XML pseudo-standard and `flakyFailure` merge semantics: https://docs.gradle.org/current/userguide/java_testing.html
- Ant JUnit XML formatter: https://ant.apache.org/manual/Tasks/junit.html
- pugixml v1.16 release and manual: https://github.com/zeux/pugixml/releases/tag/v1.16 and https://pugixml.org/docs/manual.html

The runner covers nested JUnit suites, summary-only JUnit reports, Gradle-style
`flakyFailure`, counter conflicts, DTD/entity rejection, Playwright expected failures,
retries, global errors, interrupted results, duplicate JSON fields, oversized input,
and malformed over-depth XML/JSON.

`execution_report_summary_json` intentionally emits only the frozen aggregate counters;
the `format` and derived `result` remain in `ExecutionReportSummary` for its caller.

The XML depth proof is executable: `preflight_junit_xml` is iterative and runs before
`pugi::xml_document::load_buffer`; the runner passes 65 unmatched nested `<testsuite>`
tags through that ordering under normal and ASan/UBSan builds. It is rejected at the
64-level preflight limit, so pugixml is not asked to recurse through that malformed tree.

Intentional structural limits:

- JUnit input is validated as UTF-8 by simdjson, rejects XML 1.0 control bytes, and has one `testsuite` or `testsuites` root. Only direct
  `testsuite`/`testcase` hierarchy contributes outcomes; optional metadata and output
  nodes are ignored without being exported. Any summary-only scope that declares
  `tests > 0` is rejected as incomplete rather than inferring testcase outcomes.
- Playwright input follows the current JSON reporter shape: root `errors`, `stats`, and
  `suites`; each suite has `specs`, and each test has `expectedStatus`, `status`, and
  contiguous retry records. Unknown metadata is ignored, but the observed outcomes and
  supplied `stats` must agree.
- The JSON DOM is transient and is bounded to 4 MiB, 200k SAX nodes, and depth 64. It
  may temporarily hold ignored reporter payload bytes, but the parser and summary never
  emit attachments, stdout, stderr, error text, test names, or log content.
