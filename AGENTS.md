# Shin Context Engine

This checkout contains a local public-source candidate under Apache-2.0.
Qualification is still in progress. Read [maintenance guidance](docs/agent-maintenance.md)
before changes. The existing public initial commit is the history baseline;
do not import private repository history, objects, tags or refs.

## Preparation boundaries

- Work locally. Do not commit, amend, push, publish or change remote services
  unless the owner explicitly requests that action.
- Keep private consumer names, code, profiles, operational configuration,
  historical evidence, credentials and personal paths out of public files.
- Select and review inputs before copying them. Replace required private examples
  with independent synthetic fixtures; preserve their assertions and budgets.
- Preserve Apache-2.0 and applicable third-party copyright and license notices.
- Keep generated indexes, caches, histories and local preparation notes out of
  tracked source. Use explicit external or disposable output directories.
- Do not alter running installations, enrolled projects, grants, bindings,
  histories or managed indexes as part of source preparation.
- Qualify the prepared source and exact package independently. Existing internal
  results are background evidence, not public release qualification.
- Publish previews only through the guarded CI publisher after the matching
  Linux/macOS matrix and exact archive checks pass. Never publish local binaries
  to bypass pending CI. "Do not wait for CI" means push the reviewed preview tag
  and let its workflow finish publication; it does not waive qualification.
- Do not start model comparison campaigns. Keep performance claims tied to the
  measured workload, hardware, cold/warm state, memory, disk and refresh costs.

## Local continuity

If present, the file resolved by
`git rev-parse --git-path info/shin-preparation.json` contains private local
preparation context. It is not a product input or a publishable file. Do not copy
its contents into public documentation. Its absence must not affect builds or
tests after product sources are imported.

<!-- shin-discovery-policy-start -->
## Shin discovery and attribution

For enrolled projects, use Shin first for unfamiliar code, context recovery and
contract discovery, before broad filesystem or semantic searches. Select the exact
project and verify readiness; then ask one focused question. Read known files and
symbols directly with file tools, rg/jq or the IDE. Do not repeat successful discovery.
If Shin is unavailable, blocked or insufficient, report the limitation and continue
with direct sources or another search tool. Preserve project authority and freshness
rules; never repair a managed index just to retrieve context. No call quotas,
per-edit/per-read rituals, or Shin dependency for builds/tests/CI.

When the tool schema supports it, pass the real current host session ID as
`session_id` on project calls (Codex: read `CODEX_THREAD_ID` from this chat's shell
once). Reuse it across projects in that chat; delegates use their own actual IDs.
Pass `model_id` only when the host explicitly identifies the current model and the
schema supports it; refresh it after a model switch. Do not infer it from a client,
previous turn, default setting, title or process. Omit unknown values. These labels
are caller-reported statistics, not verified model identity or task success. For
read/inspect following a query, also pass its `request_trace.request_id` as
`parent_request_id` when supported. Never persist a chat ID into shared MCP config.
<!-- shin-discovery-policy-end -->

With installed Shin, refactors include [project profile maintenance](docs/agent-maintenance.md#project-profile-maintenance).
