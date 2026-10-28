# Tests and experiments

Run registered checks through CTest. The directory describes ownership; it does
not decide which checks run. The root [CMakeLists.txt](../CMakeLists.txt) registers
Core checks, and [observer/CMakeLists.txt](../observer/CMakeLists.txt) registers
Observer checks. Test names, timeouts and assertions remain independent of file
locations.

## Layout

| Directory | Contents |
| --- | --- |
| [core](core/) | Native Core regression tests, test drivers and their shared Windows ACL helper. |
| [integration](integration/) | CLI, adoption, agent integration, lifecycle and release-contract checks; the adoption jq helper stays with its runners. |
| [tooling](tooling/) | Tests for developer tools, reports, client hooks and installed-service diagnostics; static-analysis and leak runners. |
| [benchmarks](benchmarks/) | Performance drivers and isolated C++ experiments. Experimental headers stay beside their implementations. |
| [research](research/) | Retrieval comparisons, semantic and agent-workflow experiments, their shared Python modules and hermetic regression tests. |
| [fixtures](fixtures/) | Synthetic input projects, policies and expected results. Fixture-local runners stay beside their inputs. |
| [evidence](evidence/) | Reviewed, portable qualification records. Raw local logs and generated indexes belong outside the checkout. |

The research modules form one import cohort: paired agent runs and semantic
comparisons share preparation, policy and source-validation code. Their CTest
regressions run without downloading models or starting an agent campaign.
Optional experiments have their own explicit prerequisites, including the pinned
[semantic requirements](research/semantic-pilot-requirements.txt).

Component-specific checks stay with their owners: [Observer](../observer/tests/),
[native installer](../observer/installer/), [desktop UI](../observer/tray/),
and the test directories under `shared/`. This keeps component builds independent
of a single global test launcher.

## Run checks

On Linux/macOS, run from the repository root with an external build directory:

```sh
cmake -S . -B /tmp/shin-core-build -DCMAKE_BUILD_TYPE=Release \
  -DSHIN_CONTEXT_BUILD_SOURCE_DISCOVERY=ON \
  -DSHIN_CONTEXT_BUILD_SEMANTIC_EXPORTER=ON
cmake --build /tmp/shin-core-build --parallel 4
ctest --test-dir /tmp/shin-core-build --output-on-failure --parallel 4
```

For the Observer and its Core companions:

```sh
cmake -S observer -B /tmp/shin-observer-build -DCMAKE_BUILD_TYPE=Release \
  -DSHIN_OBSERVER_SOURCE_DISCOVERY=ON -DSHIN_OBSERVER_MCP=ON
cmake --build /tmp/shin-observer-build --parallel 4
ctest --test-dir /tmp/shin-observer-build --output-on-failure --parallel 4
```

Use `ctest --test-dir /tmp/shin-core-build -N` to list registered checks, or `-R` to select
one by name. A direct shell runner still accepts the arguments documented in its
usage message; invoke it from `tests/integration/`, `tests/tooling/` or
`tests/benchmarks/` as appropriate. See [CONTRIBUTING](../CONTRIBUTING.md) for
verification and source-only boundaries.

### Native Windows checks

Configure and build with the [Windows presets](../docs/building.md#native-windows-development-builds).
Keep `SHIN_BUILD_ROOT` set to the same absolute external directory when running
CTest. From the repository root, `ctest --preset windows-native-smoke` selects
the native Core subset. From `observer/`, `ctest --preset windows-desktop` runs
the complete standalone Windows registration; `-N` lists its inventory.

The full desktop suite requires an unelevated interactive session, symlink
creation rights for refusal fixtures, and a protected CMake executable for the
machine-image check. Read the build guide's prerequisites before running it.
`windows-desktop-smoke` selects the hosted-runner subset and does not replace
the full local suite or ZIP/MSI and installed SCM qualification.

`shin-windows-package-version` exercises MSI version ordering, numeric bounds,
invalid-version refusal and reproducible product identities on every platform.
It is also in Windows smoke CI. The separate `windows-kit` preset disables
source-test registration; qualify its exact ZIP/MSI with the procedure in
[Windows source status](../docs/windows-desktop.md#installer-presentation-checks).

Monitor render checks write PNGs and `result.json` under the external build's
`tray/render-check/` directory. Overview, Projects and Models have 30 connected
or retained-disconnected frames and 15 first-run frames without a service reply.
There are also 20 documentation-warning frames.
The latter cover EN/RU project lists and details at all five viewport sizes.
Inspect representative images as well as the nonblank and action-bounds checks;
the compact layout intentionally shortens text while retaining the full report.

### Shared refresh regressions

`shin-local-refresh-races` runs the same native scenarios on Windows and POSIX.
The Windows fixture creates an owner-private temporary directory; POSIX keeps
its `mkdtemp` fixture. Initial bootstrap, automatic refresh, cached queries,
documentation-warning retention/clearing and strict refusal cases are shared.

The [runner](../observer/tests/run_refresh_race_tests.py) starts three isolated
process groups: documentation, races and safety. Core's stdout capture and all
mutable fixtures stay process-local. The groups share a 40-second deadline;
the enclosing CTest timeout remains 45 seconds and reserves three processors.
A failed group or expired deadline fails the test. Each native group retains
the original retry floors, assertions and resource limits.

Use CTest's `-R '^shin-local-refresh-races$'` filter for the full regression.
For diagnosis, the native test executable also accepts the profile-registry path
followed by `--group documentation`, `--group races` or `--group safety`.
Invoking it with only the profile path runs every group serially and is not
a substitute for the bounded CTest run.

## Adding a check

- Choose the directory by responsibility. Keep native test support with the
  tests that own it, and keep mutually importing Python modules together.
- Register automated checks in the owning component's CMake file. Benchmarks and
  operator probes do not become release gates merely by residing in `tests/`.
- Put reusable synthetic input under `fixtures/`; retain a local runner when it
  depends on that fixture's shape. Do not move fixtures to match every launcher.
- Resolve repository inputs from the source root, rather than the caller's
  working directory. Update source locators in scripts and machine contracts
  when moving a runner.
- Use disposable external state for indexes, installations, reports and model
  assets. Preserve safety, failure, concurrency and resource assertions when
  changing a check.

### Windows release artifacts and macOS status item

`tools/qualify_windows.py` verifies MSI tables and compares the administratively
extracted MSI payload with the ZIP, then runs `windows_kit_test.py` without
elevation. `tools/qualify_windows.ps1` supplies a disposable standard account on
GitHub-hosted VMs only. It never installs an MSI or enrolls a real project. The
result binds archive hashes, source commit and run attempt for the guarded
publisher. `tools/test_publish_preview.py` rejects incomplete platform matrices,
wrong provenance, changed receipts and payloads before release visibility.

The EN/RU `shin-tray-update-window-*` native AppKit regressions also verify the
fixed status-item width across update states, hidden-anchor window fallback,
rendering ownership, focus changes, close/reopen and icon visibility restoration.
They do not prove that macOS will never clip a menu-bar extra.
