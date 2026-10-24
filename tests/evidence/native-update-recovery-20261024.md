# Native update recovery: dev.33 local candidate

Platform: macOS arm64. Core `2.0.5-dev.4`, Observer/CLI/MCP
`2.0.5-dev.33`, Monitor build 57. Ad-hoc signed local candidate; this is not
evidence of a published release, HTTPS delivery, or another platform.

## Reproduction and result

[The independent fixture](../../observer/installer/previous_update_test.py) uses
real dev.28 executables (Core dev.3), one canonical project and one bootstrap
project. Its retrieval corpus contains only synthetic documents. It publishes
through the existing native updater and grants, then changes a source during
final qualification to force an actual activation rollback. The injected source
change is cleared when rollback begins.

With the unchanged dev.32 candidate, the next update fails before replacement:
`installation-recovery-command` caused by `service-project-denied`. The previous
service is running; the registered canonical project has a
`project-check-failed` enrollment verdict and no project RPC route. Independent
installation readback confirms the preserved previous kit and completed recovery.

With the dev.33 candidate extracted from the exact ZIP below, the same retry
succeeds while that service is running. Neither service-stop nor project
re-enrollment is performed by the fixture during recovery. Both projects become
ready. The native receipt confirms the target version, complete integrity and no
pending recovery. Registry, identity, canonical publication grant and binding
authority are preserved. Bootstrap migration changes only its permitted Core and
prepared-generation fields. Existing request rows retain their values; refresh
aggregate counters do not decrease and their coverage metadata is retained.

The forced failure now retains the failing root's opaque hash and the
`worktree-not-current` leaf reason. Diagnostics remain bounded to 1024 bytes;
additional gates may be explicitly truncated.

## Validation

- Version-pinned RPC and enrollment policy tests reject unknown roots/refusals,
  registry conflicts, stopped or explicit-root services, and identity/engine/
  integrity failures. Quality failures are not made globally transient.
- Thirteen affected CTest checks pass, including update controls/process/window,
  progress transport, startup, catalog/language and native EN/RU rendering. A
  broader 138-test run passed 136 checks and exposed a dictionary ordering error;
  the two affected catalog/language checks pass after its correction.
- All six package-smoke stages pass against the exact archive: EN/RU GUI proof,
  relocated retrieval, native installation, update/rollback recovery, bootstrap
  installer, and EN/RU terminal statistics/restoration.
- A separate real dev.28 → dev.33 rollback/retry passes against that archive.
- Five rendered viewport sizes in EN/RU include the last project page. Its
  visible range and total are also exposed through accessibility.

Archive: `shin-local-2.0.5-dev.33-macos-arm64.zip`

SHA-256: `d090c6c673fa0f7c5a024faeb773d0765382fe50b1046ed0e1a14043d3ad71ad`

All installation fixtures use disposable homes. No live installation, consumer
project, managed index or client configuration is repaired or replaced.
