# Building from source

For the complete macOS application, follow the [local kit guide](local-kit.md).
Core can be built alone for development or embedding. Linux and WSL2 can also
build the local service, CLI, terminal monitor and MCP without the desktop app.

## Dependencies

Use a C++20 compiler and CMake 3.24 or newer. Tests require Python 3.11 or newer,
Bash, and jq. Local source discovery requires SQLite with FTS5, version 3.38 or
newer. Observer also requires OpenSSL Crypto.
CMake downloads pinned dependencies and checks their hashes.

macOS CI uses Xcode 26.3 on macOS 15. The C++20 standard library must provide
`std::jthread` and stop tokens; the Xcode 15 toolchain is insufficient. Select
the intended Xcode installation before configuring CMake so its compiler and
SDK headers are used together.

The native Sand terminal monitor links the pinned FTXUI 7.0.3 statically.
CMake fetches its checked archive; macOS/Linux need no curses or terminfo
development package. Its MIT license is included in native installations.
No Python or JavaScript runtime is required to run the TUI.

## Core

Run from the source checkout with an explicit external build directory:

```sh
cmake -S . -B /tmp/shin-core-build \
  -DCMAKE_BUILD_TYPE=Release \
  -DSHIN_CONTEXT_BUILD_SOURCE_DISCOVERY=ON
cmake --build /tmp/shin-core-build --parallel 4
ctest --test-dir /tmp/shin-core-build --output-on-failure
```

To build an index directly, choose an external, unmanaged output directory:

```sh
/tmp/shin-core-build/shin-context --root /path/to/project \
  --profile generic-source --profile-dir ./profiles \
  --out-dir /path/to/external-index build
```

The low-level Core CLI defaults to the project root if `--out-dir` is omitted.
Native CE instead enforces separate source and state directories. An index
managed by Observer must be refreshed by its publisher, not by a manual build.

## Linux and WSL2 terminal tools

On Ubuntu 24.04, including WSL2, install the build and test prerequisites:

```sh
sudo apt-get update
sudo apt-get install -y build-essential cmake ninja-build pkg-config \
  libssl-dev libsqlite3-dev python3 jq git
```

From the source checkout:

```sh
cmake -S observer -B /tmp/shin-observer-build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DSHIN_OBSERVER_SOURCE_DISCOVERY=ON \
  -DSHIN_OBSERVER_MCP=ON \
  -DSHIN_OBSERVER_TRAY=OFF \
  -DSHIN_OBSERVER_LOCAL_KIT=OFF
cmake --build /tmp/shin-observer-build --parallel 4
ctest --test-dir /tmp/shin-observer-build --parallel 4 --output-on-failure
```

This produces `shin-ce`, `shin-observer`, `shin-mcp` and `shin-usage` in the
build directory. For a new project, use an explicit private data home:

```sh
/tmp/shin-observer-build/shin-ce init --root /path/to/project \
  --home /path/to/private-data --profile-dir ./profiles
/tmp/shin-observer-build/shin-ce stats --home /path/to/private-data
```

The project folder stays separate from that home. A source build does not
install the commands, replace another kit or enable login startup. In WSL, use
a UTF-8 interactive terminal such as Windows Terminal. The Linux route has no
native desktop Monitor or prebuilt local-kit download yet.

## Continuous integration

CI qualifies standalone Core and Observer independently on Linux and macOS.
The macOS Observer job also builds Monitor and qualifies the exact kit archive.
Core checks in that kit exercise the embedded build configuration; they do not
replace the standalone Core job.

Configure, compilation, tests and package qualification are separate workflow
steps so their durations are visible. Ninja schedules compilation with four
jobs. CTest runs up to four isolated suites concurrently, starting the longest
lifecycle suites first. Install tests share a resource lock because CMake writes
one build-tree install manifest; native layout renders also share a lock.
The short client-hook deadline suite runs alone to preserve its real one-second
process deadline without unrelated CPU contention.
Timeouts, assertions, resource bounds and the complete test inventory remain in
force. The archive is uploaded only after its own installation and UI checks.

CI uses [ccache](https://ccache.dev/manual/latest.html) for compiler outputs,
partitioned by operating system, architecture and component. Compiler contents,
flags and source/header inputs still determine cache validity. Build trees,
test state, indexes and qualification results are never restored from this cache.
A cache miss runs a normal build; a cache hit still runs every test. Cache
statistics are printed separately. Superseded runs on the same branch or pull
request are cancelled automatically.

To use the same bounded test scheduling locally, add `--parallel 4` to the CTest
command. Ninja and ccache are optional development tools, not application runtime
dependencies. Compare cold and warm CI runs separately; job time also includes
failure/recovery scenarios that deliberately wait for deadlines and watchers.
