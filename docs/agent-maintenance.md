# Development maintenance

[AGENTS.md](../AGENTS.md) defines the current preparation boundary.
[Architecture](architecture.md) describes component ownership. Runtime and wire
contracts live under `docs/machine`; client procedures belong to their integration
skill. Avoid duplicating those procedures into this document.

## Source and state

Use Core and Observer version files independently. Rebuild affected
companions after shared Core changes. Keep test databases, generated publications,
identities and telemetry outside source inputs. An explicit build does not
authorize installation or changes to any running service.

For release version changes, synchronize `developmentLane.coreVersion` and
`developmentLane.observerSourceVersion` in `docs/machine/shin-development-v1.json`
with the component version files. Run `shin-context-universality` before tagging.

Installation examples must point to a published release with its complete kit,
checksum and installer assets. Update those pins after publication, not when a
candidate is tagged. Label screenshots and controls from unreleased source
explicitly so the current preview's capabilities stay clear.

Profiles classify current product contracts and source by component. Preserve
the declared source, byte, output and quality bounds. Fixture changes must retain
the positive and negative behaviors that motivated each test. Historical
measurements do not qualify a new source tree or binary.

Self-development scopes are declared by the
[component profile contract](machine/shin-component-profiles-v1.json). UI work
uses `shin-ui`: terminal, shared presentation helpers, supported Monitor sources
and colocated fixtures. CI checks the complete eligible UI inventory as well as
retrieval gates. Native Objective-C++ hosts and include fragments still require
direct reads; declaring an extension is not parser support. For MCP, verify the
exact checkout root independently: a passing self-profile does not enroll a
worktree or change an existing connection.

## Consumer profile maintenance

When Shin is installed and the project is enrolled, the agent performing an
authorized refactor maintains its project-owned retrieval profile alongside the
change. Follow the [profile maintenance procedure](../integrations/codex/skills/shin-context/references/setup-and-adoption.md#project-profile-maintenance):
review coverage and affected checks, preserve broad queries and quality bounds,
distinguish obsolete contracts from engine misses, and complete native approval
and current-route verification. Without installed Shin this creates no setup,
build or CI requirement. The same procedure ships in the Hermes and MCP guidance.

## Verification

Build with CMake and run the relevant CTest suite. For shared changes, include
companion tests. `make release-check` also runs sanitizer, static analysis,
contract, profile ownership, performance and portable installation checks.
Set `CONTEXT_OUT` explicitly to an external unmanaged directory for targets that
build the self index. Report missing prerequisites and failures without skips or
raised limits.

Qualify a package after extracting its exact archive. Check component identities,
dependency notices, relative documentation links, runtime dependencies, source
path disclosure and preserved data in a disposable installation. Keep private
raw diagnostics outside the public repository. Scope any performance statement
to its workload, hardware and measured cold/warm state.
