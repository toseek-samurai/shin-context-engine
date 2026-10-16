# Retrieval Failure Playbook

Use for a reproducible Shin miss, failing quality gate, or bounded investigation.
For ordinary product work, follow the main skill; do not run a release audit for
every query. Resolve Core contracts using its source or installed layout.

## Capture One Reproducible Observation

Retain the exact query/request, argv array, binary version and hash, generator,
profile bytes/hash, root, output key, snapshot identity and freshness. Separate
normal indexed retrieval from an explicitly requested source scan. Keep raw
reports outside the consumer; sanitize them before sharing.

Use `eval --failed-only` or `pack-eval --failed-only` for triage. Preserve the
full failed case when needed to inspect omitted details. A failing command is
evidence, not a reason to silently substitute a passing invocation.

## Classify Before Editing

| Observation | Next check | Potential owner of a correction |
| --- | --- | --- |
| Wrong root, registry, key or generator | Adoption marker and exact route; `ready` / `worktree-diff` | Consumer adoption, not ranking |
| Path absent from publication | Existence, profile inclusion, extraction and explicit `build` | Source, profile or neutral extractor |
| Indexed path absent from candidates | `diagnose`, normalized groups, posting completeness and decoded rows | Core only if a general admission invariant is broken |
| Candidate present but not selected | Exact row identities, local ranking, diversity and output limits | Selector defect or separately reviewed selection policy |
| Returned path but answer absent from preview | Returned range, child sections, actual JSON keys | Explicit bounded source read; not invented coverage |
| Work ceiling exceeded | Measured work on identical source/profile inputs | Measured optimization or explicit budget review |
| Safety, provenance or freshness failure | Named diagnostic and independent source evidence | Fix the cause; no quality-score or budget waiver |

For a known path, use `shin_diagnose_context`. The corresponding Core CLI is:

```text
<bin> --profile <id> --profile-dir <registry> --root <root> --out-dir <out> diagnose '<original query>' --expected-path <relative-path> --require-current
```

Use `search` for an exact symbol. For owner discovery, issue a separate query
containing the canonical relative path and needed section. Do not replace the
original failing natural-language question with it in an evaluation.

## Verify What Was Actually Found

- Read the authoritative source at returned ranges. Path presence is not a
  complete answer. A heading is not all its children; a JSON overview is not
  every state or property in the object.
- Count source reads and follow-up queries as additional work. A second call
  does not retroactively make the first call complete.
- Document-wide matching must not lend unrelated member matches to one source
  fact. Preserve record type, exact origin, ranges and each provenance edge.
- Static test definitions are not passing test results. Executed evidence needs
  a trusted, current host-produced receipt. Neither proves deployment by itself.

## Make One Bounded Correction

Keep the original repro and independent regression cases. A confirmed Core bug
gets a consumer-neutral regression; aliases, owned paths and product policy
remain in the consumer profile. Correct stale paths, broken documentation or
missing source content at their owner, when authorized.

An acceptance change needs an explicit review of the question and current
answer-bearing sources. Record the old requirement, replacement, rationale and
remaining limitation. Keep useful original owner/type assertions in separate
checks where appropriate. Report this as policy review, not a ranking fix.
Do not lower required ranks, remove negative assertions or enlarge output
budgets just because a new result is convenient.

When several cases share a failure mechanism, test that mechanism and replay
the unaffected cases. Do not add one Core exception for each product question.

## Measure Performance Without An Endless Investigation

Compare old and new executables with identical source content, profile and
request. If generators differ, build separate private publications from those
same inputs; do not give one reader another generator's index. Confirm actual
content hashes, not merely matching filenames or row counts.

Keep setup/build, snapshot opening, retrieval, selection, projection and output
costs separate. Check work counters, result identity and output bytes alongside
latency. A no-index counter is not necessarily zero work. Do not run overlapping
benchmarks or builds that contaminate timing, or competing Make builds in one
checkout.

For suspected growth, vary iteration count and inspect peak RSS and repeated
request identities. A bounded plateau is useful evidence, not universal proof
of leak freedom; sanitizer checks remain independent.

Agree on a stopping condition. A small, stable latency difference with unchanged
work/output and no demonstrated safety or growth defect does not justify days
of speculative tuning. Budget revisions require documented measurements and
owner approval. Historical headroom formulas were release-specific decisions, not
an automatic rule for future failures.

## Controlled Kanban Handoff

Use this shape when a controller commissions a Shin investigation:

```text
Objective: one falsifiable question or bounded correction.
Baseline: exact source commit/dirty state, binary, generator, profile and snapshot.
Read first: current contracts and the relevant previous evidence, not all history.
Allowed edits: explicit paths; state read-only when no edits are authorized.
Must preserve: original query/request, assertions, output limits and safety rules.
Deliver: inspected source ranges, commands/results, classified cause, minimal diff
         when authorized, regression comparison and any unresolved uncertainty.
Stop when: the hypothesis is proved/falsified, an agreed resource bound is reached,
           or a named external input/approval is required.
Review: worker completion means evidence is ready for controller review; only the
        designated controller accepts integration or release.
```

Reuse a prior session/run for the same bounded problem when supported and useful.
Revalidate its baseline after source, profile or generator changes; a warm model
context is not a current index. Carry a concise verified handoff rather than the
whole transcript. Keep parallel workers on disjoint scopes; do not give multiple
workers the same mutable checkout or publish target.

A blocked card should name the actual dependency, last completed command and
smallest next action. Do not call failed/empty work done, restart unchanged
investigations indefinitely, or treat file timestamps as proof of useful output.
Use the board's native task/comment/run APIs for progress and acceptance.

## Close The Loop

Review the diff and record results before integrating. Separate source acceptance,
portable release gates, installed binary, skill delivery and canonical consumer
cutover. Before a rebuild inspect `impact` where the old snapshot is readable;
then explicitly build the same marker-owned key, run the consumer's complete
validation route and require fresh readiness. Never silently migrate an index.

Respect repository-specific write restrictions. Read-only retrieval does not
authorize source or adoption edits; release approval does not grant unrelated
product, production or provider authority. Keep detailed recipes in skills and
source-backed evidence, not in a global memory entry masquerading as architecture.
