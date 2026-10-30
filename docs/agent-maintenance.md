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
retrieval gates. Current Core source supports native Objective-C++ hosts and
Cocoa method implementations in `.h` fragments. Arbitrary `.inc` fragments still
require direct reads; declaring an extension is not parser support. For MCP, verify the
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

## Homebrew packaging

The [Homebrew distribution](homebrew.md) is built with a fixed stable opt prefix.
Only that package's bundled profile registry may retain an alias in private
bindings; Observer resolves it to the current physical package for file reads.
All other source/state paths retain canonical identity checks. MCP and Monitor
locators verify their package generation before generating or using stable paths.
Homebrew owns updates and service registration; native kit receipts and native
Updater commands must not take over a Cellar payload. Keep package builds and
install/reinstall tests isolated from existing native kits and enrolled projects.

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

On a generator cutover, retire its predecessor in the shared source-rebuild
policy and update the pinned previous-generator kit fixture. Exact macOS archive
qualification exercises that published format through forward update and resumed
rollback. Bundled bootstrap profile changes have a separate journaled contract;
changing generator identity does not approve arbitrary scope or budget changes.

Applying a qualified kit update also authorizes its bounded generator-pin
transition for already approved managed project routes. The installer journals
both marker revisions, preserves every byte except the generator value, and
uses native `marker-accept`. Rollback restores the original bytes and approval
through the previous CLI. Pending owner edits are never included in this
approval. Ordinary agent work and Core reads still cannot modify or approve
markers. Qualify updates with a real pinned adoption marker, not only markerless
routes; preserve native review failures and interrupted rollback coverage.

<!-- shin-profile-maintenance-start -->

## Project profile maintenance

Applies only when Shin Context Engine is installed and this exact project is
already enrolled. During an authorized refactor, the project agent also reviews
the project-owned Shin profile; do not wait for a user-reported search warning.
Without installed Shin, continue ordinary work without installing/enrolling it
or adding a build/CI dependency.

When documents, rules, packages, source roots or contracts change, check affected
inclusions/exclusions, agentStartPaths, expected paths and eval/Pack cases.
Prepare equivalent profile updates with the refactor. Diagnose failures against
current source first: distinguish a moved/obsolete owner or changed contract
from a retrieval regression. Preserve natural-language task intent, useful broad
queries, negative assertions, rank/coverage requirements and resource limits.
Add current terminology as coverage; an exact identifier query does not replace
a failing broad query. Explain any owner-reviewed retirement of obsolete cases.

Never delete failures, relax checks, raise limits, index evaluation answers or
move required cases into advisory/history suites just to make status green.
Keep confirmed engine misses visible with original queries, expected owners,
versions and bounded diagnostics; exclude private source/secrets from reports.

A profile edit is not runtime approval. Review its exact native
shin-ce profile-review diff; scope, limits, safety and quality policy need
explicit owner authority for that change. Existing explicit authorization may
cover acceptance; do not ask again when it does. Do not approve through MCP or
re-enroll, switch indexes or manually rebuild a managed index.

After approval, let the service refresh on the canonical route. Run affected
strict eval/Pack checks and the full applicable profile suite before claiming
quality restored; verify current status and a representative query/read with
its fresh snapshot. If Shin is unavailable, report pending validation. Handoff
records profile changes, checks and unresolved warnings; edits alone prove no recovery.
<!-- shin-profile-maintenance-end -->
