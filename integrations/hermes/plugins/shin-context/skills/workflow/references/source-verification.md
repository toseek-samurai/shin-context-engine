# Source Verification

Read when a preview is incomplete, evidence conflicts, or identity, freshness or provenance affects a decision. The resolved Core contracts own semantics; these are source-reading procedures, not authority upgrades.

## Verify Exact Ranges

A heading-only Markdown range is a container, not its contents. Inspect its
child headings and bound an explicit source read at the next same-or-higher-level
heading. For a truncated JSON preview, use a structured parser to read the
requested keys from the named source; a filename or root summary is insufficient.
Keep these reads within the task's remaining byte/line budget and report any
missing content. This is additional source evidence and cost, not expanded Shin
coverage or a pass of the original single-call gate.

## Trust Boundaries

For the detailed policy, read `trust-boundary-hardening-v1.json` from the
resolved Core contract directory. In this workflow:

- Verified scope is transported retrieval support, not truth, deployment,
  approval, execution, or product authority.
- Pack deduplication is exact-only `canonical-record-v1`; matching text or path
  does not permit merging records with different stable metadata. Preserve all
  match edges and exact verification ranges.
- Missing or malformed cache state is a read-only verification fallback. Hermes
  never repairs or publishes it. An explicit Core `build` is required to warm a
  current publication, followed by the consumer's strict validation route.
- Older-generator publications require an explicit rebuild with the current
  binary at the same canonical external index. The current generator is
  `shin-generator-v21-cpp-syntax`; release approval is separate from an
  installation, deployment, or consumer-cutover receipt. Do not silently switch
  indexes, overlay files, execute commands, compact context, or publish implicitly.

## MCP Failure Guidance

If advertised, `shin_help` explains safe recovery and escalation; pass only an
optional `error_code`, never a project path or source text. It is static help,
not a readiness check. Follow `recovery.kind` and `responsible_party`: correct
requests, retry once at a useful checkpoint, inspect project evidence within task
authority, ask the owner/operator, or report to Shin maintainers. Missing evidence
does not prove a deleted file. `maintainer-diagnosis` with `cause_status=unconfirmed`
means the cause is unknown, not a confirmed engine bug. Include only reviewed code,
bounded diagnostics, versions and opaque correlation IDs; omit secrets and source
bodies. A disconnected adapter cannot serve help. Never weaken gates for recovery.

## Choosing the right reader

For enrolled projects, query Shin when entering unfamiliar code, recovering context
or finding a contract. Known paths/symbols use direct reads, rg/jq, IDE tools and tests.
Do not duplicate successful searches. Give narrow delegates paths and evidence;
request fresh discovery only when needed. No per-read calls or usage quotas.

SOURCE follow-ups require the same project and query snapshot. Document paths in
`document_context` are not SOURCE bindings: use authorized ordinary readers for docs
and build files without SOURCE locators. Refusal does not prove a file is absent.
After snapshot mismatch, query the same project once; do not retry the old read.

Select each repository's allowed handle; single-root MCP cannot serve other roots.
Use a verified existing CLI route or authorized direct reads; never guess homes/grants.
Pass real host session IDs only, with the query's request ID for follow-ups.
Missing correlation is unknown. Never infer model identity, success or token savings
from call counts; client token usage and Shin requests measure different things.
