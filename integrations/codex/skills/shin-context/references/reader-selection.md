# Choosing the right reader

For enrolled projects, use Shin first for unfamiliar code, context recovery or
contract discovery, before broad filesystem or other semantic searches. Known paths/symbols use direct reads, rg/jq, IDE tools and tests.
Do not duplicate successful searches. Give narrow delegates paths and evidence;
request fresh discovery only when needed. No per-read calls or usage quotas.

SOURCE follow-ups require the same project and query snapshot. Document paths in
`document_context` are not SOURCE bindings: use authorized ordinary readers for docs
and build files without SOURCE locators. Refusal does not prove a file is absent.
After snapshot mismatch, query the same project once; do not retry the old read.

When returned, `source_scope` lists the profile's source roots/extensions and
limits; omitted-entry counts make truncation explicit. It describes source
search, independently of document evidence and readiness. An empty result does
not establish absence outside this scope. CSS/SCSS, HTML and Razor text can carry
exact `bounded-text-fallback` locators without evaluating cascade or templates.
Use direct readers for uncovered files; engine misses do not require rewriting
project documentation or relaxing a project profile.

Select each repository's allowed handle; single-root MCP cannot serve other roots.
Use a verified existing CLI route or authorized direct reads; never guess homes/grants.
Pass real host session IDs only, with the query's request ID for follow-ups.
Missing correlation is unknown. Never infer model identity, success or token savings
from call counts; client token usage and Shin requests measure different things.

## Request attribution

When the tool schema supports it, pass the real current host session ID as
`session_id` on project calls (Codex: read `CODEX_THREAD_ID` from this chat's shell
once). Reuse it across projects in that chat; delegates use their own actual IDs.
Pass `model_id` only when the host explicitly identifies the current model and the
schema supports it; refresh it after a model switch. Do not infer it from a client,
previous turn, default setting, title or process. Omit unknown values. These labels
are caller-reported statistics, not verified model identity or task success. For
read/inspect following a query, also pass its `request_trace.request_id` as
`parent_request_id` when supported. Never persist a chat ID into shared MCP config.

## Pending reader status

A status with `query_available=true` and `next_action=shin_query` permits one useful
query while `local_ready=false`. `project_state=qualified` with
`reader_state=warming` means its in-memory reader is starting.
`project_state=refreshing` with `reader_state=waiting-for-qualification` means
background qualification is pending. Submit the useful query once; it waits
automatically for up to 20 seconds. Status polling and manual sleeps add no
readiness proof. Real blockers keep their typed error and recovery guidance.

## Snapshot convenience

When the advertised schema permits it, omit a follow-up snapshot only after a
successful query with the same real session_id and project on this connection.
The adapter pins it; explicit snapshots stay strict. Concurrent queries or a new
connection require the exact returned snapshot. Inspect context_result when
available: found evidence is not an assessed answer. Engine misses do not require
restructuring project documentation.
