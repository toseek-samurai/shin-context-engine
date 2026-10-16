# Route And Publication

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
