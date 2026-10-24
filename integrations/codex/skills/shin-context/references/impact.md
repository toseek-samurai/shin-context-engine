# Change-Aware Impact

Use a supported Shin2.0 publication: the current generator is
`shin-generator-v22-compact-lexical`; `shin-generator-v21-cpp-syntax` remains
readable. An unsupported format requires an authorized migration of the same
canonical output. A read does not migrate or manually rebuild a managed index.

Use the reviewed profile, root and canonical external index:

```bash
<shin-bin> --profile <profile> --profile-dir <profile-dir> --root <consumer-root> --out-dir <output> impact --limit 24 --depth 1
```

The command compares the profile-owned current inputs with the last published
snapshot. It does not need Git, execute actions, publish a snapshot, mutate the
consumer, or store task/chat state. Run it before rebuilding, because rebuild
replaces the comparison baseline. `resume` recommends this inspection when its
published index is stale; it never runs the command automatically.

Inspect `changes`, `reread`, `coverage` and `next_actions`. Changed files come
first, then declared dependency neighbors to the requested depth (maximum two).
`source_state` and each scope's `basis` distinguish current observed bytes from
historical snapshot ranges. Java and TypeScript/JavaScript use valid syntax
declarations; other supported languages retain explicitly lexical ranges.
`span_hash` compares whole physical source-line bytes, not behavior or symbol
identity. A moved unchanged span does not prove unchanged meaning.

The graph contains explicit documentation links, recognized imports, physical
includes and named structured references. It is not a compiler call graph.
Relations from changed sources retain their published-snapshot basis and may
no longer describe the current source. Missing and ambiguous references are not
silently resolved. New imports are not a complete incremental graph overlay.

`tests_to_review` means statically related test sources, never passed tests or
automatically selected executable commands. Review the project-owned gates.
`coverage.selection_complete` concerns the bounded path selection, not all
semantic consequences. `evidence_complete` also checks baseline probe bounds,
reread limits, syntax observation and per-path scope/relation omissions.
`inspection_complete` requires both; `--strict` fails when it is false.
Inspect unlocalized changes separately. `--require-current`
additionally requires the published snapshot to match the worktree, so normally
omit it when explicitly investigating unpublished changes.

After reviewing the differences, let Observer refresh a managed publication and
confirm native readiness before a fresh query. For an explicitly unmanaged
publication, rebuild the same reviewed output only with publication authority,
then rerun `ready --strict` before current Brief/Pack retrieval.
