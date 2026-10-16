# Setup And Adoption

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

For the native2.0 service, normal setup is one authorized `shin-ce init` and
subsequent `shin_status`/query/inspect/read. Existing adopted routes need their
reviewed Observer configuration and explicit publication authority. Do not run
the Core publication procedure below on a service-managed index. It is reserved
for an explicitly requested first migration or unmanaged Core2.0 operation.

## Working And Historical Scopes

For an authorized cleanup or adoption, follow
`project-adoption-v1.json` section `informationArchitecture.knowledgeLifecycle`.
Keep the working route authoritative for normal readiness. History needs an
explicit owner-documented profile and distinct external key, not a fallback to
a passing cache. A separate rank-advisory report can remain red without making
required source, safety or freshness checks optional. Only the owner can move
an expectation between those scopes; agents cannot reclassify a failed gate.

Read when no binary/route is resolved, on an unadopted project, or during an explicitly requested permanent adoption. Skip for a ready, already-resolved route. The main skill defines placeholders; the selected Core contracts remain authoritative.

## Locate Shin

Resolve one executable and one profile registry without searching the whole filesystem:

1. Use executable `$SHIN_CONTEXT_BIN` and registry `$SHIN_CONTEXT_PROFILE_DIR` when both are set.
2. Under `$SHIN_CONTEXT_HOME`, accept either source layout `build/shin-context` plus `profiles`, or installed layout `bin/shin-context` plus `share/shin-context/profiles`.
3. If `shin-context` is on `PATH`, use it with `$SHIN_CONTEXT_PROFILE_DIR` or its sibling installed `share/shin-context/profiles` directory.
4. Use a source layout only when the task explicitly selects the active2.0 checkout.

Use an already built qualified kit. Do not compile the engine as a repository-start
side effect. If no executable and profile registry pair exists, report that Shin is unavailable.

Use these placeholders below:

- binary: `<shin-bin>`
- profiles: `<profile-dir>`
- consumer root: the repository currently being investigated
- output: `$SHIN_CONTEXT_CACHE_DIR/<index-key>` when set, otherwise `${XDG_CACHE_HOME:-$HOME/.cache}/shin-context/<index-key>`

Generated output must remain outside the consumer repository. A bootstrap route
may use the temporary profile id as its key. Permanent adoption must use the
validated marker `index.key` exactly; do not substitute the profile id or project
id after cutover.

Never write generated Shin output into the consumer repository during bootstrap.

## Respect Existing Adoption

Before cold start, check only these conventional project-local entry points when they exist:

- `AGENTS.md`
- `docs/machine/shin-project-adoption-v1.json`
- `docs/machine/shin-context-engine-adoption-v1.json`
- `docs/shin-context-engine-adoption.json`
- `docs/architecture/shin-context-engine-adoption.json`

An explicit adoption marker takes precedence over generic discovery. For `shin.project-adoption.v1`, validate it first:

Resolve the adopted profile registry without a broad scan. If
`<consumer-root>/docs/machine/shin-profiles/<profile.id>.json` exists, use that
project-owned registry. Otherwise use the registry resolved with the Shin
executable. Never merge registries or copy a consumer profile into Shin Core.
Use the same selected registry for adoption, build, validation, and retrieval.
If several output locations exist, establish the canonical base from the
project's adopted instructions or validation helper, not from which index is
newer or passes readiness. Unresolved ownership requires clarification.

```bash
<shin-bin> --profile-dir <profile-dir> --root <consumer-root> adoption-check --marker <marker-path> --strict
```

Use its permanent profile and derive the external output from `index.key`. Treat the
directory as `<external-output-base>/<index.key>`. Audit any existing Make,
shell, CI, IDE, or documentation helper before trusting it: the helper must not
retain an older bootstrap/profile-derived key and must build that canonical index,
then run `validate --marker <marker-path> --strict`. Add `--summary` for normal
agent use when successful nested gate payloads are unnecessary; failed gates
retain bounded reason, evidence, omission accounting, and remediation fields.
Run the named gate directly or omit `--summary` only when that compact evidence
is insufficient. This aggregate command retains
the required `adoption-check --strict`, `profile-check --strict`, check,
`doctor --strict`, dry-run vacuum, `eval --strict`, and `pack-eval --strict` results
without stopping after the first failure. A helper may expose the same gates
individually, but must not omit one. A passing ad hoc command does not close adoption when the
project-owned route still points elsewhere or omits a gate. Continue with cold
start only when no adopted Shin contract exists.

## Cold-Start Workflow

1. Run discovery without a profile:

```bash
<shin-bin> --root <consumer-root> discover
```

2. Read `bootstrap.state`, `bootstrap.indexable`, `bootstrap.primary_profile`, and `bootstrap.project_mutation_required` from the JSON result. Require project mutation to remain false during bootstrap.

3. If `indexable` is false, stop. Report that no supported source or documentation exists; do not invent context or create documentation automatically.

4. Validate the selected profile:

```bash
<shin-bin> --profile <primary-profile> --profile-dir <profile-dir> --root <consumer-root> profile-check --strict
```

5. Build and verify the external index:

```bash
<shin-bin> --profile <primary-profile> --profile-dir <profile-dir> --root <consumer-root> --out-dir <output> build
<shin-bin> --profile <primary-profile> --profile-dir <profile-dir> --root <consumer-root> --out-dir <output> check
<shin-bin> --profile <primary-profile> --profile-dir <profile-dir> --root <consumer-root> --out-dir <output> doctor
```

Bootstrap profiles publish broken local links as explicit diagnostics so an imperfect unfamiliar repository remains retrievable. Require `doctor` to report zero errors, surface every warning, and do not promote that profile while warnings remain. `doctor --strict` intentionally fails on those warnings and remains mandatory for permanent adoption.

## Profile Selection

Trust `bootstrap.primary_profile`. Built-in bootstrap profiles are:

- `generic-java`
- `generic-kotlin`
- `generic-node`
- `generic-cpp`
- `generic-dotnet`
- `generic-retool`
- `generic-source` for mixed-language repositories
- `generic-docs` when only documentation is recognized

Bootstrap profiles intentionally have no project-specific golden eval cases. Do not run `eval --strict` against them or present them as permanent project contracts.

## Core And Integration Boundaries

Treat the binary, generic profiles, portable machine contracts, and release
gates as Shin Core. A permanent consumer profile is optional integration policy,
not evidence that the consumer's product semantics belong in Core. Integration
ownership in a source checkout is declared by
`docs/machine/universality-boundary-v1.json` and the manifests under
`integrations/*/shin-integration-bundle-v1.json`. Those integration bundles are
not part of an installed portable distribution; installed users supply their own
permanent profile registry and adoption marker after bootstrap.

For an unrelated repository without an adoption marker, start with `discover` and the selected
`generic-*` profile. Do not copy aliases, eval queries, paths, federation members,
or authority rules from an existing integration bundle. Promote only the policy
that the new repository has established and can prove with its own eval cases.

When maintaining Shin itself, `make check`, `make perf-check`, `make safety`, and
`make release-check` are portable Core gates. Run the corresponding
`ecosystem-*` targets only when optional local integration repositories are
available or their profiles changed.

## Promote Adoption

### MDC Rules

When a project keeps rules in `.mdc`, inspect its declared rule directory during
adoption. Shin does not discover hidden directories such as `.cursor/rules`
automatically. Enroll the canonical files in the consumer-owned profile, using
explicit `rootFiles` where appropriate; do not create parallel `.md` copies.
Shin2.0 reads enrolled `.mdc` through the Markdown pipeline and preserves
original paths and line ranges. It does not execute `globs` or `alwaysApply`,
or grant a rule authority because of its extension. Those boundaries are owned
by `lexical-retrieval-v1.json` section `markdownPublication` and
`project-adoption-v1.json` section `informationArchitecture`.
After authorized enrollment, let the managed service refresh the same index; confirm the
manifest includes the files as Markdown, and run the project's strict
validation with a representative rule query. Do not manually rebuild a granted index.

### Permanent Route

After the project establishes stable contracts, create a consumer-specific profile with explicit roots, limits, topics, `agentStartPaths`, golden `evalCases`, and at least one representative `packEvalCases` task. Before authoring the marker, request a deterministic read-only promotion plan:

```bash
<shin-bin> --profile <permanent-profile> --profile-dir <profile-dir> --root <consumer-root> adoption-plan --marker <marker-path> --agent-entry AGENTS.md
```

Require `schema_version` to be `shin.adoption-plan.v1`, inspect every `gaps`
entry, and proceed only from `marker-authoring-ready`. The planner never writes
consumer files and `readiness.adoption_validated` remains false. Add a
location-independent `shin.project-adoption.v1` marker only when the user asks
to adopt Shin permanently, then require `adoption-check --strict`.

After marker validation:

1. Read the validated `index.key`; choose one external output base and form
   `<external-output-base>/<index.key>`.
2. Replace or retire older bootstrap/profile-id output routes.
3. Provide one project-owned validation target that builds the canonical index and
   runs `validate --marker <marker-path> --strict`, or a documented sequence with
   the same marker, profile, index, health, eval, and Pack-eval gates.
4. Run that project-owned route from start to finish. Permanent adoption is not
   complete merely because the same commands passed separately.

Permanent eval cases may add bounded `queryVariants`, `maxCandidateRows`, and
`minCoveredGroups` so reordered or natural task phrasings cannot silently weaken
the profile while candidate work remains measurable.

Pack eval cases keep product meaning in the profile and may assert completion,
required and forbidden paths, evidence and compact-output ceilings, and
serial/parallel identity. Run `pack-eval --strict` after engine or permanent
profile changes; do not move those expectations into C++ Core.

Do not fork Shin, embed consumer business semantics in its core, or replace source verification with generated context.
