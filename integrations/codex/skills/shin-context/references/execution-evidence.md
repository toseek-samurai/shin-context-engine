# External Execution Evidence

Optional Core2.0 workflow. Use it only when
the host has an explicitly authorized external test or check and a local producer
can provide bounded metadata. In an installed skill, read
`<prefix>/share/shin-context/contracts/execution-evidence-v1.json` and
`<prefix>/share/shin-context/contracts/execution-receipt-v1.json`. In a source
checkout, the corresponding files are `docs/machine/execution-evidence-v1.json`
and `docs/machine/execution-receipt-v1.json`.

## Three Steps

1. Capture the pre-run scope outside the consumer and generated index:

   ```bash
   <shin-bin> --profile <profile> --profile-dir <profile-dir> --root <root> --out-dir <output> \
     receipt-scope --input <execution-input-request.json> --task-id <task-id> --thread-id <thread-id> --require-current
   ```

   After exit zero, validate stdout's `schema_version` as
   `shin.execution-inputs.v1` and persist it atomically outside the consumer.
   This data document has no universal `ok` field. Use `profile` coverage for
   whole-profile receipts. Use `declared-complete` only with explicit,
   non-overlapping file/tree selectors and an explicit producer declaration;
   Shin does not infer completeness from imports. A missing explicitly selected
   path is a negative observation; Git tracking is not required. Additions or
   removals inside a tree invalidate its observation.

   Minimal scope request (`execution-input-request.json`):

   ```json
   {
     "schema_version": "shin.execution-input-request.v1",
     "coverage": "partial",
     "selectors": [{"path": "src/selected.ts", "kind": "file"}]
   }
   ```

   Prefer `coverage: "profile"` with `"selectors": []` or the conservative
   `"partial"` example above. Use `declared-complete` only when the trusted
   producer has explicitly accounted for test fixtures, configuration,
   dependencies and execution artifacts as well as the changed source;
   selecting one changed file does not establish completeness.

2. The host independently executes the authorized test or check. The producer
   must bind the exact captured inputs, declare `complete=true`, supply a
   sanitized command fingerprint, result and timestamps, and protect the local
   report. Shin does not execute the command. Source isolation is the producer's
   responsibility; pre/post hashes are not continuous execution attestation.

   The host owns the fingerprint convention. Prefer SHA-256 of compact UTF-8
   JSON containing the sanitized argv array, with no trailing newline. Do not
   join arguments with spaces: different argv boundaries can produce the same
   string. Remove secrets before hashing; the hash is not execution attestation.

3. Import one local JUnit XML or Playwright JSON report, then assess it:

   ```bash
   <shin-bin> --profile <profile> --profile-dir <profile-dir> --root <root> --out-dir <output> \
     receipt-import --input <receipt-import-request.json> --task-id <task-id> --thread-id <thread-id> --require-current
   <shin-bin> --profile <profile> --profile-dir <profile-dir> --root <root> --out-dir <output> \
     receipt-check --input <receipt-bundle.json> --task-id <task-id> --thread-id <thread-id> \
     --trusted-producer <producer-id> --strict
   ```

   After exit zero, validate `shin.receipt-bundle.v1` and persist the bundle
   atomically; retain the previous valid artifact on any failure.

   Import retains only normalized counters, format, byte count, content hash
   and producer metadata. It retains no names, errors, logs, screenshots,
   attachments or report paths. `receipt-check --strict` requires a nonempty
   receipt array and every receipt to be `passed_current`; `--require-current`
   separately checks KB freshness. Do not treat either flag as execution or
   trust authorization.

   Complete import request (`receipt-import-request.json`):

   ```json
   {
     "schema_version": "shin.receipt-import-request.v1",
     "format": "junit",
     "report_path": "/tmp/shin-reports/junit.xml",
     "inputs_path": "/tmp/shin-evidence/execution-inputs.json",
     "receipt_id": "test-run-2026-09-05T120000Z",
     "producer": "local-ci",
     "started_at": 1788609600,
     "observed_at": 1788609660,
     "ttl_seconds": 86400,
     "command_fingerprint": "fnv1a64:0123456789abcdef",
     "complete": true
   }
   ```

   All eleven keys are required. `schema_version`, `format`, paths, ids,
   `producer` and `command_fingerprint` are non-empty strings;
   `format` is `junit` or `playwright`; timestamps and `ttl_seconds` are
   integers; `complete` must be the boolean `true`. Use
   `captured_at <= started_at <= observed_at <= now`, a regular non-symlink
   report path, and a lowercase `fnv1a64:<16 hex>` or `sha256:<64 hex>`
   fingerprint. Unknown extra request keys are invalid.

   The import accepts one local report. JUnit is a conservative UTF-8 XML
   allowlist with no DTD/DOCTYPE or ENTITY; unknown dialect elements, attrs or
   contradictory counters reject. Non-empty summary-only JUnit with non-zero
   counters rejects. Playwright is likewise validated against its bounded
   JSON dialect and declared stats. Mixed pass/skipped cases produce
   `unknown`; only normalized counters are retained. Browser screenshots have
   no semantic validation meaning and are never retained.

   `--require-current` is optional on both `receipt-scope` and
   `receipt-import`; omit it when KB freshness is not part of the requested
   gate. It is independent from producer trust and receipt pass/fail.

## Applicability And Embedding

Root, profile, profile configuration, task and thread are fixed. For `profile`
coverage, `inputs_current` checks the entire binding. For `partial` coverage,
`inputs_current` checks only selected inputs and does not alone imply
`scope_current`; fixed binding and whole-profile applicability still apply.
With explicit `declared-complete` inputs, worktree/snapshot drift outside the
selected inputs may be tolerated; selector drift invalidates the receipt. A
missing explicitly selected path is a negative observation; Git tracking is
not required. Generic manual build and service receipts remain valid without
these optional fields.

Resume and Impact consume the same assessment. A checkpoint or Impact result may
embed the bounded receipt and receipt-derived `next_actions`, but receipt
failures remain separate from static `tests_to_review` and Impact strict static
inspection completeness. Static source or test evidence is never upgraded by
an imported receipt. Profile discovery and fallback are unchanged.

This workflow is manual and read-only: no automatic execution, compaction hook,
daemon, MCP endpoint, index publication or consumer write is implied.
