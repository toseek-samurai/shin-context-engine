# Route And Publication

## Project Profile Maintenance

Applies only when Shin Context Engine is installed and this exact project is
already enrolled. The agent performing an authorized code/document refactor owns
the accompanying profile review. Without installed Shin, continue ordinary work;
do not install or enroll it, or add a build/CI dependency, to satisfy this rule.

At a relevant refactor checkpoint, not on every read, review the project-owned
profile when documents, rules, source roots, packages or contracts move or change.
Check enrolled roots/files, exclusions, `agentStartPaths`, expected paths and
affected `evalCases`/`packEvalCases`. Prepare equivalent updates in the same task;
do not wait for the owner to notice a search warning.

Diagnose a failed query against current source before editing expectations.
Distinguish an intentional owner/path/contract change from a retrieval regression.
Map moved paths to their current owners and retain the original task intent,
natural-language queries, negative assertions, rank/coverage requirements and
resource limits. If terminology changes, add the current wording while retaining
useful broad wording as a regression case. An exact identifier query is additional
coverage, not a replacement for a failing broad query. Retire a genuinely obsolete
scenario only with an explained owner-reviewed contract change.

Never remove a failing case, relax rank/coverage requirements, increase limits,
enroll evaluation answers, or move a required check into an advisory/history suite
just to make status green. A confirmed engine miss stays visible with its original
query, expected owner, versions and bounded diagnostic for Shin maintainers.
Keep private source and diagnostics out of public reports.

Preparing a profile edit is not accepting it. Follow [Profile Change Review](#profile-change-review);
changes to scope, limits, safety or quality policy require explicit owner authority
for the exact reviewed change. Existing authorization may cover acceptance; do not
ask again when it does. MCP reads grant no approval authority.

Use the installed kit and canonical route for affected strict eval/Pack checks,
then the full applicable profile suite before claiming quality restored. Never
manually rebuild a managed index: after acceptance let the service refresh, verify
current status and a representative query/read with its fresh snapshot. If the
tool is unavailable, report pending validation. Handoff states what changed,
which checks passed and any remaining warning; profile text alone is not recovery.

## Profile Change Review

A confirmed blocked refresh requires a prompt user-visible notice: name the
project and safe reason code, state that current Shin context is unavailable,
and disclose any direct-source fallback. Carry an unresolved blocker into the
handoff; successful application tests do not restore Shin readiness.

For `route-revision-conflict` / `recovery.kind=operator-review`, editing the
profile has not completed the coverage change. Report the pending review and
continue direct-source work. Review the exact change in Shin Monitor (View changes)
or with read-only `shin-ce profile-review` using the verified root/home. An owner
must accept it; act on existing explicit authorization only if it covers that exact
reviewed change. If review is unavailable, report the limitation for operator
review. MCP cannot approve it. Do not retry or reconnect as a substitute for
approval. After acceptance, let the service qualify the index, then make a fresh
same-project query and use its new snapshot; acceptance alone is not readiness.

Native2.0 uses `shin_status`, `shin_query`, `shin_inspect` and `shin_read`.
The terminal/Core details below apply only to explicitly authorized advanced
maintenance, not as a fallback or normal per-task rebuild procedure.

## Working And Historical Scopes

The default marker-owned route serves current work. An owner may define a
complementary history profile and external key for explicit archaeology; it is
not an alternate cache to try after a failure. When the native adopted-route
surface does not expose that complementary profile, use the exact reviewed CLI
route, read-only unless publication was separately authorized. Do not override
the working marker to make a historical command pass.

For scope maintenance, use `project-adoption-v1.json` section
`informationArchitecture.knowledgeLifecycle`. Historical and rank-advisory
failures stay visible in separate reports; required source, safety, freshness
and working quality failures remain blockers. Agents cannot demote a failed gate.

Read for an unknown project, failed freshness, explicit adoption, or an authorized rebuild. Use the machine-contract directory resolved by the main skill; this procedure never grants write authority.

## Ambiguous Route

For `AMBIGUOUS_INDEX_ROUTE`, read the project's adopted instructions or
build/validation helper to establish the external output base. Only then pass
that declared `out_dir` explicitly. A candidate being newer or passing readiness
does not establish ownership. If no project-owned declaration resolves the
choice, report the ambiguity and request clarification; do not try every index
and continue with the first success.

Likewise, `index-unavailable` for a default location does not prove that a
different project-declared publication is absent. Use the declared `out_dir`
and check it; do not create a new index at the default location as a repair.

## Stale Context

When readiness fails because the worktree differs from the published snapshot:

1. Inspect native `shin_status` and the service diagnostics.
2. Open the reported changed paths directly.
3. Let the managed service refresh. Do not manually publish its index.
4. Run the consumer's documented read-only service check.
5. Call `shin_status` again; report a persistent refusal.

Never silently switch to another index, relax freshness, or scan the whole
repository to hide a failed readiness gate.

## Unknown Project

For explicit unmanaged Core discovery, read
`bootstrap.indexable`, `bootstrap.primary_profile`, and
`bootstrap.project_mutation_required`.

- Stop when the repository is not indexable.
- Keep bootstrap output outside the consumer repository.
- A permanent adoption marker always wins over generic discovery.
- Do not create project policy, documentation, or a permanent profile without
  an explicit adoption task.

For a new permanent adopter, read `project-adoption-v1.json` section `informationArchitecture` and `adoption-planner-v1.json` before writing a marker. Preserve the consumer's conventions and validated index key. Adoption is complete only after the project-owned build/validation route passes, not after isolated commands.

## MDC Rules During Adoption

If the project declares a rule directory such as `.cursor/rules`, inspect it
explicitly: hidden rule directories are not automatically discovered. Register
the original `.mdc` files in the consumer-owned profile (`rootFiles` can name
them explicitly), without mirrored `.md` files. Shin2.0 uses Markdown
extraction with the original ranges; it does not execute `globs`/`alwaysApply`
or treat the extension as authority. Exact semantics remain in the resolved
`lexical-retrieval-v1.json` section `markdownPublication` and the shared
`informationArchitecture` policy.
Enrollment and publication require the authorized terminal workflow, not a
read-only MCP call. The managed service refreshes the same external index.
Check Markdown entries in the manifest, a representative rule query, and the
project's strict validation. A passing generic bootstrap does not establish
that these project rules were included.
