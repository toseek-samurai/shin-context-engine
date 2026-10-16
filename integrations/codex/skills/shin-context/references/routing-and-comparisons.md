# Routing And Comparisons

Read only for an explicitly cross-project task, a reviewed routing configuration, or comparison of published snapshots. A route recommendation is not permission to switch scope. Reuse the selected binary and contract directory from the main skill.

## Profile Routing

When the current repository explicitly names a reviewed
`shin.profile-routing.v1` config and the task may belong to another project or
component, ask for a route before retrieval:

```bash
<shin-bin> suggest-profile --config <routing-config> --from-root <consumer-root> '<task query>'
```

Follow a result only when `state` is `recommended` or `current-route`, then use
the returned root, profile, profile registry, and project adoption contract
through the normal strict workflow. For `ambiguous` or `no-match`, do not guess,
merge indexes, or treat the current profile as an implicit fallback. The command
is read-only advice: `automatic_switch`, `index_access`, and `project_mutation`
must remain false. Do not search for routing configs outside established agent
entry points.

## Cross-Project Tasks

When one task explicitly crosses several adopted repositories, use a reviewed
`shin.federation.v1` config and query their already published indexes without
merging them:

```bash
<shin-bin> federated-brief --config <federation.json> '<task query>' --require-current
```

Treat `member_ids`, per-member freshness, `local_rank`, and audit paths as part
of the evidence. Raw scores are member-local and must not be compared as
cross-profile authority. Do not invent implicit federation members or place a
federation config into a consumer repository unless its owners adopt that
cross-project route.

When a reviewed federation also provides a release quality suite, validate it
before trusting the route after profile or engine changes:

```bash
<shin-bin> federated-eval --config <federation.json> --suite <federation.eval.json> --strict
```

Do not invent product expectations in an ad hoc suite; suite paths and queries
are owned configuration and remain outside C++ Core.

Use `snapshot-diff --from <old-output> --to <new-output>` only for already
published local indexes. It verifies both integrity tables before reporting
metadata, source, generated-file, and syntactic-record deltas; it is not a
semantic source diff.

When an agent must verify whether a repository or profile change altered the
context for one reviewed implementation task, retain the previous output root,
publish the candidate output root, and run:

```bash
<shin-bin> --profile <profile> --profile-dir <profile-dir> --root <consumer-root> pack-diff --from <old-output> --to <new-output> --input <task-request.json> --require-current --strict
```

Inspect required requirement regressions before evidence additions, removals,
rank changes, or match-edge changes. `snapshot_changed=true` with
`agent_context_changed=false` means the repository changed without changing the
bounded evidence selected for that task. The historical source tree is not
required; exact freshness applies only to the target snapshot.
