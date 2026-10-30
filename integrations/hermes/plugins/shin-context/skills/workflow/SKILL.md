---
name: shin-context
description: Retrieve verified Shin project context after context loss or before broad investigation; maintain enrolled project profiles after refactors. Read known files directly.
version: 2.0.0
platforms: [macos, linux]
metadata:
  hermes:
    tags: [software-development, context, retrieval, verification]
    category: software-development
---

# Shin Project Context

Use Shin first for unfamiliar code; read known files directly.
Read [reader selection](references/source-verification.md#choosing-the-right-reader) for context/read recovery.

Require Shin v2.0.0 or newer with `shin-generator-v24-objective-cpp`.
Older indexes require rebuilding. Local use needs no login. Configure native MCP separately;
a Python plugin does not replace it.

1. Read the repository's `AGENTS.md` chain and adoption marker. Do not
   add `.hermes.md` to override that route or infer the root from a chat title.
2. If `shin_projects` exists, choose the exact host-allowed workspace and pass
   its `project` handle in project calls. Otherwise the adapter pins one root.
   Call `shin_status` and verify the returned root. Hosts may prefix tool names.
3. Use `shin_query` for a focused question. Follow with `shin_inspect`/`shin_read`
   using the same `project`, returned `mcp_snapshot` and project-relative path
   in multi-project mode; single-root mode uses the native `snapshot_id`.
4. Verify consequential claims in current source. Scope and omissions matter;
   a green status or selected path does not prove a complete answer or executed test.

Handoff and correlation: [session guidance](references/session-and-execution.md).


On failure, follow `recovery`; use `shin_help` if advertised.

Keep parallel project calls independent; label evidence by root/workspace. A
catalog row's `binding_active` is not query readiness. Never guess a selector or
reuse another project's snapshot. Reconnect after a lifecycle/identity-change
refusal; it does not grant re-enrollment or permission to alter client settings.

The service updates granted indexes automatically, including uncommitted files
and non-Git projects. A standby project remains observed and wakes its reader on
demand; it does not require another init. Freshness combines background
qualification and selected-source checks, not a synchronous whole-corpus scan.
For `source_coverage.state=partial`, report omissions and use verified context
when `retrieval_ready=true`, even if `canonical_ready=false`. Do not claim full
coverage. Changed/deleted/quarantined files stay withheld until qualified refresh.

Report blockers by project/reason; use direct sources and retain them in handoff.
`route-revision-conflict` requires owner review. Editing a profile is not approval.
Follow [profile maintenance and review](references/route-and-publication.md#project-profile-maintenance)
after refactors; verify readiness after acceptance.

A preparing status is pending: submit the useful `shin_query`. It waits up to
20 seconds inside one call. No manual sleeps or status polling. After that wait
expires or a real blocker appears, use direct sources; return at a useful
checkpoint. No call quotas.
Do not guess another home or
select a source-checkout binary. Missing setup is not permission to bootstrap
over a canonical marker. `init`, detach, profile/marker approval and grants are
explicit owner operations; MCP navigation and reads grant no new access.

## Conversation And Compression Boundary

The Hermes integration contract leaves conversation memory and compression to
Hermes. Shin supplies verified project evidence across that boundary, without
instruction or product authority. Read-only retrieval cannot write skills or
change model/client settings.

## Conditional References

Use only for the named explicit task, never as a failed-native fallback:

- [Route and publication](references/route-and-publication.md): authorized adoption and maintenance.
- [Source verification](references/source-verification.md): truncated/ambiguous evidence and trust.
- [Retrieval playbook](references/retrieval-playbook.md): misses and commissioned evaluation.
- [Session and execution](references/session-and-execution.md): explicit checkpoints and execution evidence.

Resolve the selected Core2.0 contracts from `<shin-home>/docs/machine` for its
active source checkout or `<prefix>/share/shin-context/contracts` in a portable
kit. Use `project-adoption-v1.json`, `lexical-retrieval-v1.json`,
`trust-boundary-hardening-v1.json` and `execution-evidence-v1.json` as appropriate.
No credentials, gateway restart, automatic continuation or forced Shin use is
implied. Graphify and other context engines are optional, not dependencies.
