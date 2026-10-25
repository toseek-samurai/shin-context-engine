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

From the repository root, select a build directory:

```sh
cmake -S . -B build/core -DCMAKE_BUILD_TYPE=Release \
  -DSHIN_CONTEXT_BUILD_SOURCE_DISCOVERY=ON \
  -DSHIN_CONTEXT_BUILD_SEMANTIC_EXPORTER=ON
cmake --build build/core --parallel 4
ctest --test-dir build/core --output-on-failure --parallel 4
```

For the Observer and its Core companions:

```sh
cmake -S observer -B build/observer -DCMAKE_BUILD_TYPE=Release \
  -DSHIN_OBSERVER_SOURCE_DISCOVERY=ON -DSHIN_OBSERVER_MCP=ON
cmake --build build/observer --parallel 4
ctest --test-dir build/observer --output-on-failure --parallel 4
```

Use `ctest --test-dir build/core -N` to list registered checks, or `-R` to select
one by name. A direct shell runner still accepts the arguments documented in its
usage message; invoke it from `tests/integration/`, `tests/tooling/` or
`tests/benchmarks/` as appropriate. See [CONTRIBUTING](../CONTRIBUTING.md) for
verification and source-only boundaries.

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
