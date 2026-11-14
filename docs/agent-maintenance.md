# Development maintenance

[AGENTS.md](../AGENTS.md) defines the current preparation boundary.
[Architecture](architecture.md) describes component ownership. Runtime and wire
contracts live under `docs/machine`; client procedures belong to their integration
skill. Avoid duplicating those procedures into this document.

## Source and state

New kit installations rebuild managed indexes, reader caches and qualification
state from current project sources using that kit's rules. Preserve enrollment,
project-owned profiles and approvals, agent settings and history. Derived data
from an earlier kit is disposable, even when its source files did not change.
Do not add historical index readers, version allowlists or incremental migrations
of old derived results. Keep source and authority fences and transactional
installation recovery. Pending and blocked projects remain visible in Monitor.

Recover owned derived-state damage and interrupted preparation automatically.
Keep retry/coalescing bounds; never solve a block by discarding enrollment,
approvals or history, raising project limits, or disabling safety checks.
For operator-dependent failures, show the cause and an appropriate action:
review settings/search checks, inspect project access, review storage, or copy
a diagnostic report. Show the next step in compact Monitor and TUI as well.

Use Core and Observer version files independently. Rebuild affected
companions after shared Core changes. Keep test databases, generated publications,
identities and telemetry outside source inputs. An explicit build does not
authorize installation or changes to any running service.

For release version changes, synchronize `developmentLane.coreVersion` and
`developmentLane.observerSourceVersion` in `docs/machine/shin-development-v1.json`
with the component version files. Before tagging, run both
`shin-context-universality` and `shin-context-codex-skill`; the latter also checks
the root agent-entry byte budget after maintenance-policy edits.
Budgeted `AGENTS.md` and `SKILL.md` entrypoints use LF on every platform so
checkout newline conversion cannot change those byte counts.
The large shared `observer/tray/monitor_view.h` header also uses LF so a Windows
checkout remains within the same source-file byte budget as the canonical input.

Before tagging a Core or Observer change, also build the native Observer with
source discovery and MCP enabled and run its complete CTest suite with
`--parallel 4 --output-on-failure --no-tests=error --output-junit <external-report>`.
On macOS use the Monitor and local-kit options from
[the kit build procedure](local-kit.md#build-the-package).
This matches the source-test workload in CI. Focused tests are regression proof;
the full suite also covers the service, MCP, lifecycle and installer contracts.
Use an external build directory dedicated to source checks and original
development dependencies. Keep relocated libraries from extracted kits in
archive fixtures. Record the component versions, compiler, Python and CMake
options with the results.
Keep package qualification tied to the exact archive, independently of source tests.
For asynchronous failures, force the relevant ordering with existing process or
publication controls in disposable fixtures. Preserve both the waiting call and
later-call behavior, including bounded diagnostics and automatic recovery.

Automatic source qualification runs on pull requests and `v*` tag pushes.
Ordinary branch pushes, including `main`, do not start another matrix. Use the
manual workflow for an untagged direct change that needs hosted checks. A release
tag performs its platform tests, exact packaging and guarded publication once.
Maintainers can use [Release preview](releasing.md) to prepare the next Observer
version and dispatch that same pipeline for its immutable tag. Manual tag runs
retain the source, attempt and exact-package gates; branch runs cannot publish.

Compiler reuse is optional and never qualifies source or a package. Version
strings are linked from generated `.cpp` files, rather than public compiler
definitions; changing only a version must not recompile its consumers. CMake
must also leave unchanged generated headers untouched.

The CI compiler cache uses ccache on Linux/macOS and sccache with MSVC/Ninja
Multi-Config on Windows. Interactive Windows Visual Studio presets remain
available. The Windows Core job builds the executables required by its existing
smoke suite; the desktop job independently qualifies Observer, Monitor and the
exact ZIP/MSI.

Ordinary [GitHub caches](https://docs.github.com/en/actions/reference/workflows-and-actions/dependency-caching#restrictions-for-accessing-a-cache)
cannot be read by sibling tags. To reuse compiler data
across releases, successful component jobs export bounded, seven-day artifacts
after their source and package gates. Imports check the repository, immutable
tag/SHA, workflow, run attempt, successful component job and toolchain identity.
PR jobs never export these artifacts. Cache lookup, download or validation
failure means a cold build. Keep per-platform publication independent and keep
all source tests and exact-archive gates mandatory. Record cache hits and build
time separately from test/package time; a warm local build is not hosted timing
or Windows runtime proof.

Installation examples must point to a published release with its complete kit,
checksum and installer assets. Update those pins after publication, not when a
candidate is tagged. Label screenshots and controls from unreleased source
explicitly so the current preview's capabilities stay clear.

The [documentation index](README.md) owns the current platform capability matrix.
Keep download pins in README, local-kit and Windows guides aligned only after
publication. UI actions must describe the supported platform and package owner:
native Updater, Homebrew and Windows MSI have different lifecycles. A source
feature, hosted render or successful build alone is not installed-platform proof.
Release notes retain their historical version; current guides must not call an
already shipped feature unreleased. Contract baseline identities are explained
in the [machine documentation guide](machine/README.md).

The native kit ships only `local-kit.md` and `native-client-setup.md` as operator
guides. Keep their mutual/anchor links relative; links to unshipped guides and
contracts must use repository URLs. Run `tools/check_package_docs.py` against the
exact package to verify those links; source-tree link checks alone cannot do so.

Profiles classify current product contracts and source by component. Preserve
the declared source, byte, output and quality bounds. Fixture changes must retain
the positive and negative behaviors that motivated each test. Historical
measurements do not qualify a new source tree or binary.

The Core self-profile's required agent entrypoints refer to current source files,
including `profiles/shin.json` for input ownership, start paths and retrieval
checks. They must validate before a self index exists. After building, inspect
`docs/kb/generated/manifest.json` under the explicit external `CONTEXT_OUT` for
generated counts and snapshot metadata. Do not copy that artifact into the
checkout to satisfy source qualification. Profiles that explicitly require a
generated start path still require it under their selected output root.

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

An installed stress probe must select current enrolled roots without enrolling,
resetting or manually rebuilding them. Label calls as diagnostic with the real
host session; omit unknown model identity. Cover sequential and concurrent
queries, languages, snapshot-bound reads and invalid-input controls. Record
documented capacity refusals, source-change races and readiness timeouts
separately from invalid data or process failures, and verify the service drains
after load. Keep consumer queries, source paths and raw responses private.
Synthetic fixtures, rather than consumer profiles, preserve reproduced bugs.

The optional background qualification cache also yields under process descriptor
pressure. Its native watches may retain ancestor directories. Evicting that cache
restores full qualification; it does not waive freshness or raise OS/profile
limits. Exercise deep multi-project roots under a bounded descriptor limit and
keep original resource kind, numeric OS error and the actual soft limit in bounded
source-observation diagnostics. Connection overload must remain an explicit
admission refusal with no request execution or automatic replay.

Compare delivery quality at identical snapshot, profile, result and per-document
limits. An eval request with eight records and an MCP response with four records
are different workloads. A full record budget does not itself prove complete
document coverage. Preserve the original expected owners and profile checks
when investigating delivery misses; do not weaken them to match a smaller view.

Exact macOS archives also exercise the pinned previous published release in
`.github/fixtures/macos-previous-release.json`: a changed project must remain
pending without blocking a same-Core upgrade, while a changed approved profile
must still refuse it. Keep that pin on a published predecessor. If Core changes, this gate exercises
its real forward transition and interrupted rollback instead; synthetic same-Core
coverage remains in the native suite. The separate
previous-generator suite remains mandatory for format transitions.

On a generator cutover, update the pinned previous-generator kit fixture and
qualify a complete source rebuild across formats, including resumed rollback.
The fixture is regression evidence, not a runtime list of supported old formats.
Kit-owned bootstrap policy is prepared under the installation journal; a new kit
never replaces project-owned canonical profiles or their approvals.

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

Applies only when Shin is installed and this exact project is enrolled. The
project owns source selection and product scenarios; Shin owns retrieval algorithms,
execution budgets, regression fixtures and diagnostics. Ordinary code/document
edits do not create a separate Shin maintenance task.

Review the profile when a selected root, owning path or product contract actually
moves or changes. Update affected paths and scenario assertions alongside that
refactor; retain useful natural-language queries, negative assertions, rank/coverage
requirements and resource limits. Do not reorganize otherwise sound documentation,
repeat query wording in headings, or split files to reduce search counters.
`mustEvidence` and `mustNotEvidence` can describe the selected fragment, statement
and lifecycle status; matching a file alone does not establish a useful answer.

When current sources still satisfy the intended scenario and Shin misses it,
continue authorized product work with direct sources. Preserve the original query,
expected fragment/owner, version and bounded diagnostic for Shin's backlog. Engine
regressions and resource-counter warnings belong to Shin maintainers; consumers do
not tune algorithms, limits or wording until the indicator turns green. Keep
private source, profiles and diagnostics out of public reports.

Never remove failures, weaken assertions, raise limits, enroll evaluation answers
or move required checks into advisory/history suites to hide an engine miss.
Retire obsolete scenarios only for an explained, owner-reviewed product change.
Scope, safety and approval rules still apply: a prepared profile edit is not
runtime acceptance. Use native `shin-ce profile-review`; act on existing explicit
authority only for the exact reviewed change. MCP grants no approval authority.
Never re-enroll, switch indexes or manually rebuild a managed index.

After an approved profile change, let the service refresh on its canonical route.
Run the affected checks and the applicable full suite before claiming quality
restored, then verify a representative query/read with the fresh snapshot. If
unavailable, record pending validation and continue product work. Handoff records
remaining engine misses; application tests do not prove Shin recovery.

<!-- shin-profile-maintenance-end -->
