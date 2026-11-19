# Choosing the right reader

For enrolled projects, use Shin first for unfamiliar code, context recovery or
contract discovery, before broad filesystem or other semantic searches. Known paths/symbols use direct reads, rg/jq, IDE tools and tests.
Do not duplicate successful searches. Give narrow delegates paths and evidence;
request fresh discovery only when needed. No per-read calls or usage quotas.

SOURCE follow-ups require the same project and query snapshot. Document paths in
`document_context` are not SOURCE bindings: use authorized ordinary readers for docs
and build files without SOURCE locators. Refusal does not prove a file is absent.
After snapshot mismatch, query the same project once; do not retry the old read.

When `source_scope.selection=automatic-project-text`, Shin searches eligible
text under the registered folder, including unusual and extensionless files.
An empty `extensions` list in this mode means extensions do not restrict
selection. Use the returned scope, limits and omissions. Structured evidence
is available for supported formats. Other text can have exact
`bounded-text-fallback` locators. Use direct readers when evidence is missing.
An empty result does not prove absence.

Select each repository's allowed handle; single-root MCP cannot serve other roots.
Use a verified existing CLI route or authorized direct reads; never guess homes/grants.
Pass real host session IDs only, with the query's request ID for follow-ups.
Missing correlation is unknown. Never infer model identity, success or token savings
from call counts; client token usage and Shin requests measure different things.

On large automatic indexes, the SOURCE header can include `file_coverage`.
When `matching_files_complete=false`, only the reported file window was
examined for this question. Use a distinctive term, symbol or known path for
a narrower follow-up. This limit concerns the query; registration is unchanged.

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

## Availability and quality

Honor explicit `retrieval_ready` when present. Otherwise use a successful
`ok=true`, `local_ready=true` response. Managed projects qualify current
evidence under the kit's automatic checks. Report partial coverage, redaction
and real warnings. A source entry can still be withheld after a change.
Respect authority, freshness, integrity, safety and resource refusals.

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
