# Publication And Maintenance

This is an explicit advanced Core2.0 procedure, not normal native-service usage.
Managed indexes refresh in the background; use native status and do not run the
unmanaged rebuild or pruning commands below against a granted publication.

Read when the worktree is stale, an explicit source scan is needed, or the user requests index maintenance. These are not automatic side effects of retrieval. Use the route resolved by the main skill.

## Worktree And Readiness

When active development may have changed profile-owned files after the published
snapshot, inspect exact unpublished drift before trusting the index as complete:

```bash
<shin-bin> --profile <profile> --profile-dir <profile-dir> --root <consumer-root> --out-dir <output> worktree-diff
```

This command is Git-independent and read-only. Open reported paths directly. It
does not overlay changed files into retrieval; the managed service owns refresh.
For an explicitly unmanaged publication, rebuild and rerun strict gates when the
current worktree must become indexed. Use `--strict` only as a freshness gate.

Use `impact` before rebuilding when the question is what changed or what to reread. Follow [change-aware impact](impact.md); rebuilding replaces that comparison baseline.

For a permanently adopted project, prefer one compact readiness envelope before
retrieval after the canonical index has been built:

```bash
<shin-bin> --profile <profile> --profile-dir <profile-dir> --root <consumer-root> --out-dir <output> ready --marker <marker-path> --strict
```

When a reviewed routing config is already named by the project, add
`--routing-config <routing-config> --task '<task query>'`. Require `ready=true`
and `next_action=brief-or-pack`. `ready` validates exact route, marker profile,
marker index key, worktree freshness, and the compact aggregate quality gates
against one publication boundary. It never builds, repairs, switches profiles,
retrieves context, or mutates the project. On `rebuild-index`, rebuild the same
canonical output and rerun `ready`; on a route or adoption mismatch, repair the
explicit invocation rather than opening the selected index.
When `next_action` has a deterministic remediation, inspect the structured
`next_command.argv` and mutation flags before invoking it directly. Never turn
the array into an interpolated shell command. Use `ready --verbose` only when
the compact diagnostic ids and failed-gate summary are insufficient; verbose
readiness invokes full aggregate validation.

## Explicit Source Scan

Use `--source-scan` only for an explicit live read that cannot wait for a reviewed
publication rebuild. Search, context, and brief then include profile-selected
source facts as well as documents, keep `snapshot_id=null`, and report source-fact
scan counters in `index_freshness`. With `--require-current`, capped or skipped
source-fact input fails as `incomplete-source-scan`. This mode is read-only and
does not update, overlay, or replace the canonical generated index.

## External Index Maintenance

Index lifecycle commands are operator maintenance, not part of ordinary
retrieval. Always supply the known external cache root explicitly:

```bash
<shin-bin> index-list --cache-dir <external-output-base> --strict
<shin-bin> index-prune --cache-dir <external-output-base> --keep-key <active-index-key>
<shin-bin> index-prune --cache-dir <external-output-base> --keep-key <active-index-key> --apply --strict
```

The first `index-prune` is a dry-run. Review every action before adding
`--apply`. Derive every active `--keep-key` from validated adoption markers.
Use `--prune-unreferenced` only when every active key under that cache root is
listed; it is rejected without at least one keep key. These commands inspect
only immediate verified Shin snapshots and remove only generated payloads.
They never discover cache roots, scan consumer repositories, or delete corrupt,
unsafe, unmanaged, empty, or protected entries.
