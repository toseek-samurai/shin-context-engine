# Machine-readable contracts

These JSON documents describe schemas, authority boundaries and verification
requirements. They are not evidence that a package was published, installed or
tested on a particular machine. For installation and platform capabilities, use
the [current documentation](../README.md).

## Current identities

- [VERSION](../../VERSION) identifies the current Core source.
- [observer/VERSION](../../observer/VERSION) identifies Observer, CLI, MCP and Monitor source.
- [shin-development-v1.json](shin-development-v1.json) records their independent development identities.
- [lexical-retrieval-v1.json](lexical-retrieval-v1.json) declares the current index reader/writer policy.
- [mcp-service-v1.json](mcp-service-v1.json) defines managed project discovery, source scope, MCP transport, runtime identity and reconnect boundaries.
- [shin-context-engine.manifest.json](shin-context-engine.manifest.json) describes the product and command inventory.

The current generator is `shin-generator-v25-source-redaction`. Retrieval requires
the running binary's generator; there is no reader for old index formats.
Managed publications are rebuilt from the selected folders and their current
exclusions. Standalone Core uses an explicit `build` and can use profiles, strict
quality suites and adoption markers. A read request grants no authority to rebuild
or replace a binding. See [index rebuilds](../local-kit.md#core-transitions-and-index-rebuilds).

## Historical metadata and plans

Older `release.productVersion`, `release.generator`, top-level `version` or
`generator`, `stableBaseline`, milestone and `introducedIn` fields record the
origin of a contract or extension. They do not identify the current executable,
promise an old-version compatibility reader or establish a stable public release.
Use the source version files and current reader policy above for those decisions.

`accepted` and `active-contract` describe a contract's status, not installation
qualification. A plan, requirement, acceptance list or future integration section
does not prove its implementation. Check the corresponding source, test results
and exact-package receipt. The Observer archive/delivery contracts retain local
format and historical loopback-delivery rules; no standalone Server or remote
receiver is distributed.

## Maintenance

Keep behavior contracts aligned with the implementation and retain their
historical provenance. Version changes must update current source metadata
without relabeling old evidence. The build files own target source lists,
platform selections and dependency pins; inventories summarize their stated scope.

Use the existing [agent maintenance](../agent-maintenance.md) and
[test guide](../../tests/README.md) for validation. Release-contract, universality,
profile, documentation and client-guidance checks complement runtime tests; none
substitutes for an interactive or installed-platform check.
