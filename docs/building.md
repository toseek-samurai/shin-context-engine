# Building from source

For the complete macOS application, follow the [local kit guide](local-kit.md).
The commands below build Core alone, for development or embedding.

## Dependencies

Use a C++20 compiler and CMake 3.24 or newer. Tests require Python 3.11 or newer,
Bash, and jq. Local source discovery requires SQLite with FTS5, version 3.38 or
newer. Observer also requires OpenSSL Crypto. Server requires OpenSSL 3 or newer,
libcurl 7.85 or newer, and PostgreSQL client development libraries 13 or newer.
CMake downloads pinned dependencies and checks their hashes.

macOS CI uses Xcode 26.3 on macOS 15. The C++20 standard library must provide
`std::jthread` and stop tokens; the Xcode 15 toolchain is insufficient. Select
the intended Xcode installation before configuring CMake so its compiler and
SDK headers are used together.

The native terminal monitor links curses: macOS uses the SDK/system library;
Linux Observer builds need the ncurses development package (`libncurses-dev` on
Debian/Ubuntu). No Python or JavaScript runtime is required to run the TUI.

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

## Server development

```sh
cmake -S server -B /tmp/shin-server-build -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/shin-server-build --parallel 4
ctest --test-dir /tmp/shin-server-build --output-on-failure
```

These tests do not establish database integration or remote deployment readiness.
The [Server readiness assessment](server-readiness.md) describes the implemented
capabilities, required integration proof, and remaining user workflows.

The development PostgreSQL harness uses a fixed Compose project and volume and
resets its test schema. It must run on a dedicated disposable Docker environment;
a different source checkout or `--state-root` does not isolate the database.
