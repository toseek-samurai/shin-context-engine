# Building from source

For a complete managed macOS source build, including Monitor, use the
[project Homebrew tap](homebrew.md).

For the complete macOS application, follow the [local kit guide](local-kit.md).
Core can be built alone for development or embedding. Linux and WSL2 can also
build the local service, CLI, terminal monitor and MCP without the desktop app.
Native Windows development builds provide Core, Observer, CLI, MCP and a
Win32/DX11 Monitor; their scope is described in [Windows source status](windows-desktop.md).

## Dependencies

Use a C++20 compiler and CMake 3.24 or newer. Tests require Python 3.11 or newer;
the Linux/macOS suites also require Bash and jq. On those platforms, local source
discovery requires SQLite with FTS5, version 3.38 or newer, and Observer requires
OpenSSL Crypto. Windows uses a pinned static SQLite build and native cryptography.
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

Core observes Objective-C (`.m`), Objective-C++ (`.mm`) and Cocoa headers through
a vendored C11 Tree-sitter grammar. Indexing these files needs no Cocoa SDK or
Objective-C compiler, and works independently of building the macOS application.
The `generic-cpp` and `generic-source` profiles include them automatically;
custom profiles must allow the relevant extensions. Ordinary C++ retains its
existing parser. See the [syntax scope and limits](machine/source-member-ranges-v1.json).

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

Shell runners must retain LF line endings, including when a Windows checkout is
used from WSL. The repository attributes enforce LF for project shell files.
An older checkout may still contain CRLF copies; a fresh checkout applies the
policy without changing global Git settings.

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

## Native Windows development builds

The Windows source route uses Visual Studio 2022 with the C++ desktop workload,
a Windows SDK, CMake 3.24+, and Python 3.11+. Run the commands in PowerShell with
CMake, CTest and Python available on PATH. The first target is x64 on local NTFS;
UNC paths and reparse-point traversal are refused by the Windows IO adapter.

For the full native test suite, use an **unelevated interactive desktop session**.
The user runtime rejects elevated tokens, and the Monitor checks need a desktop
with Direct3D 11 support. The filesystem refusal tests deliberately create
symlinks: the test account needs symlink creation rights, normally through
[Windows Developer Mode](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-createsymboliclinkw).
This is a test prerequisite, not a requirement for running Shin. Do not elevate
the full suite to work around a missing symlink privilege.

Use a protected CMake installation, such as the one bundled with Visual Studio
under Program Files. `shin-windows-machine-code` uses the configured CMake
executable as its machine-owned image fixture; a user-writable CMake installation
does not satisfy that check. Missing prerequisites must be reported as failures,
not replaced with a reduced test set described as full qualification.

Keep both the checkout and its absolute external build root outside AppData
and TEMP. MSBuild does not reliably track dependencies in those directories;
the build rejects them.
The presets are enabled only on Windows with `SHIN_BUILD_ROOT` set. From the
repository root:

```powershell
$env:SHIN_BUILD_ROOT = Join-Path $env:USERPROFILE 'ShinBuild/windows-development'
cmake --preset windows-native
cmake --build --preset windows-native
ctest --preset windows-native-smoke

Push-Location observer
cmake --preset windows-desktop
cmake --build --preset windows-desktop
ctest --preset windows-desktop
Pop-Location
```

The root preset builds Core, source discovery and native Observer/CE/MCP tools
under `core/Release`. Its smoke preset exercises native Core retrieval, source
reading, publication ownership, cache reuse, Unicode and long paths. It is an
explicit subset: the full Core suite still includes POSIX shell runners whose
Windows equivalents remain pending. No tests are removed from the Core suite.

The standalone Observer preset builds CE/MCP and Win32/DX11 Monitor under
`desktop/Release` and `desktop/tray/Release`, then runs all tests registered in
that configuration. Both routes use the same Core and Observer sources. They
do not install a service or enroll a real project; tests use disposable state.
Runtime binaries do not require WSL, Bash or Python.

The Windows suite also runs the shared refresh-race regressions, including
bootstrap with broken documentation links, cached queries, warning recovery,
and strict rejection of unrelated failures. Monitor renders cover the warning
in English and Russian in project lists and details at all five tested sizes.

These are development source checks, not Windows distribution qualification.
The desktop presets leave ZIP/MSI packaging disabled. The separate `windows-kit`
preset builds production binaries in `kit/`, with source tests disabled; run
the desktop test preset first, then the [exact ZIP/MSI checks](windows-desktop.md#installer-presentation-checks).
MSI packaging requires CPack 4.3+ and WiX 4; exact-archive and installed SCM lifecycle checks
are still required before a Windows release. TUI, automatic client connection,
automatic usage collection and in-app updating remain pending on Windows.

Do not run an unfiltered CTest against a manually configured packaging build
with `BUILD_TESTING=ON`: enabling the Core CLI also registers POSIX-only Core
gates. The desktop preset and the Core smoke preset are the supported native
Windows source-test inventories; neither requires Bash/jq.

## Continuous integration

CI qualifies standalone Core and Observer independently on Linux and macOS.
The macOS Observer job also builds Monitor and qualifies the exact kit archive.
Core checks in that kit exercise the embedded build configuration; they do not
replace the standalone Core job.

The same [source workflow](../.github/workflows/ci.yml) builds both Windows
presets on Windows Server 2022. Its smoke selections avoid POSIX-only drivers.
After source tests pass, the desktop job reconfigures the same build directory
with the production `windows-kit` preset and tests disabled, reusing compiler
outputs. It packages the exact ZIP and MSI with CPack 4.3.5 and
WiX 4.0.6, verifies compiled MSI policy, extracts its payload without installing
it, and compares every file with the ZIP. It runs the relocated ZIP's native
Monitor render and init/query/stop checks under a disposable standard account.
Hosted runner elevation is never used as evidence of a supported user runtime.

Both Windows jobs and all four Linux/macOS jobs are mandatory for publication.
The publisher verifies both artifacts from the same commit, tag, run and attempt;
Windows runtime evidence and all four Windows asset hashes are bound in a receipt.
These CI package checks complement the real interactive-session MSI lifecycle
rehearsal; they do not claim to repeat logon, installed SCM recovery or upgrade
rehearsals on a hosted runner. See [Windows gates](windows-desktop.md#package-publication-and-remaining-gates).

Configure, compilation, tests and package qualification are separate workflow
steps so their durations are visible. In the Linux/macOS workflow, Ninja schedules
compilation with four jobs. CTest runs up to four isolated suites concurrently,
starting the longest lifecycle suites first. Install tests share a resource lock
because CMake writes one build-tree install manifest; native layout renders also share a lock.
The short client-hook deadline suite runs alone to preserve its real one-second
process deadline without unrelated CPU contention.
Timeouts, assertions, resource bounds and the complete test inventory remain in
force. The macOS archive is uploaded only after its own installation and UI checks.

The Linux/macOS workflow uses [ccache](https://ccache.dev/manual/latest.html) for compiler outputs,
partitioned by operating system, architecture and component. Compiler contents,
flags and source/header inputs still determine cache validity. Build trees,
test state, indexes and qualification results are never restored from this cache.
A cache miss runs a normal build; a cache hit still runs every test. Cache
statistics are printed separately. Superseded runs on the same branch or pull
request are cancelled automatically.

To use the same Linux/macOS test scheduling locally, add `--parallel 4` to the CTest
command. Ninja and ccache are optional development tools, not application runtime
dependencies. Compare cold and warm CI runs separately; job time also includes
failure/recovery scenarios that deliberately wait for deadlines and watchers.

The Windows presets use Visual Studio x64 Release builds with four build jobs
and two CTest jobs. The shared refresh suite reserves three processors for its
isolated subprocess groups while retaining one 45-second CTest deadline; see
the [test guide](../tests/README.md#shared-refresh-regressions).
