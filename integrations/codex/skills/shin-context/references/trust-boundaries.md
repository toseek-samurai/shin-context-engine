# Trust Boundaries

Read this reference when a task depends on freshness, provenance, lifecycle
status, Pack selection, cache state, or release compatibility. It supplements
the short routing rules in `SKILL.md`; it does not grant execution, publication,
or consumer-repository write authority.

## Evidence Is Not Truth

`verified` describes the bounded scope that Shin inspected and transported. It
does not establish product truth, deployment state, execution, approval, or
security authenticity. A returned record is retrieval support until an agent
opens the exact `verification_paths` in authoritative source, tests, or machine
contracts. A test-source record proves that a test is defined, not that it ran
or passed. A receipt is separate producer evidence and cannot upgrade static
source evidence by itself.

Keep these axes separate:

- `status` and `authority_level` are lifecycle and repository-authority signals;
- `evidence_kind` and `verification_level` describe provenance;
- freshness and snapshot identity describe the inspected publication;
- Pack completion describes evidence coverage, not correctness of the claim.

Do not infer accepted, rejected, superseded, deployed, or executed from similar
text, a heading, a filename fragment, a score, or a complete-looking Pack.

## Source Ranges

A heading-only Markdown range is a container, not its contents. Inspect its
child headings and bound an explicit source read at the next same-or-higher-level
heading. For a truncated JSON preview, use a structured parser to read the
requested keys from the named source; a filename or root summary is insufficient.
Keep these reads within the task's remaining byte/line budget and report any
missing content. This is additional source evidence and cost, not expanded Shin
coverage or a pass of the original single-call gate.

Open the returned source paths to verify important claims. Treat `authority_level`
and `status` as ranking and triage signals: current `source-of-truth` or accepted
evidence normally takes precedence over proposal, review, and history evidence,
but an explicit proposal or archaeology query may legitimately reverse that order.
Treat `evidence_kind` and `verification_level` as separate provenance axes.
`implementation-source` means current extracted source bytes, not deployed
production code. `test-source` with `source-observed` means a test definition was
statically extracted; it does not prove execution or success. Only a trusted
receipt may justify `validated` or `executed`.


## Pack Identity

Pack deduplication uses `canonical-record-v1` and is exact-only. Two records may
share text, path, line range, preview, or normalized summary and still remain
separate when their evidence type, lifecycle, authority, verification range,
provenance, or any other stable field differs. The canonical identity is the
evidence type plus the complete stable record JSON.

When one record serves several requirements, retain one record but preserve all
requirement match edges, per-query ranks, matched fields, and query-group
indices. Never merge records by fuzzy similarity, source overlap, embedding,
path alone, or consumer-specific rules. Open every exact verification range
needed for an important claim.

## Read-Only Freshness And Cache

Read-only commands may inspect existing output, locks, and optional freshness
cache state. A missing, malformed, incompatible, or metadata-mismatched cache
falls back to read-only hashing/verification. It does not create, repair,
replace, or publish a cache, lock, index, or generated snapshot. A cache hit is
an optimization after the bounded physical layout and cache identity checks;
it is not extra authority.

Let Observer refresh managed publications. For an explicitly unmanaged output,
an authorized `build` publishes current source bytes and may warm its cache. A
snapshot-republishing `vacuum --apply` may also prepare its explicitly requested
cache state. Review the
resulting output and run the required strict checks; retrieval, `ready`,
`brief`, `pack`, and diagnostics do not publish implicitly.

## Generator Compatibility

The current generator is `shin-generator-v23-unicode-lexical`. Older indexes
are not readable. A read never rebuilds a publication. Unsupported formats
require an authorized rebuild
with the current binary at the same canonical external index, preserving the
managed publisher's ownership. Release approval is separate from installation,
deployment and consumer cutover. Keep the release-approved source policy,
freshness checks and the reviewed index; do not overlay live files or infer
compatibility from the product version alone.

## Host Boundaries

Shin Core does not execute commands, install consumer policy, or publish on an
agent's behalf. Keep generated output in the reviewed external location. Do not
add automatic compaction hooks, background repair, implicit publication, or
consumer-specific authority rules to this workflow. When a host explicitly
authorizes an external check, retain its receipt as bounded producer evidence
and keep execution, source isolation, and trust decisions at the host boundary.

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
