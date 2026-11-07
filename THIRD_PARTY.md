# Third-party components

The project license applies to Shin's code. Dependencies keep their original
licenses and copyright notices. The exact source pins and archive hashes are in
the referenced build files; system-library versions depend on the build host.

| Component | Source selection | License notices |
| --- | --- | --- |
| nlohmann JSON | Vendored 3.12.0 | `third_party/nlohmann/LICENSE.MIT` |
| simdjson | Vendored 4.6.10 | `third_party/simdjson/LICENSE-MIT`, `LICENSE-APACHE-2.0` |
| pugixml | Vendored 1.16 | `third_party/pugixml/LICENSE.MIT` |
| Tree-sitter runtime | Vendored 0.27.0 | Runtime MIT; Unicode notice in `src/unicode/LICENSE` |
| Java, C++, TypeScript/TSX grammars | 0.23.5, 0.23.4, 0.23.2 | Each grammar's `LICENSE` and `UPSTREAM.json` |
| Objective-C/Objective-C++ grammar | Vendored 0.1.0, `third_party/tree-sitter-objcpp/UPSTREAM.json` | Upstream MIT `LICENSE` |
| utf8proc | Vendored 2.12.0, `third_party/utf8proc/UPSTREAM.json` | MIT and Unicode data notices in `LICENSE.md` |
| RE2 | Source candidate 2025-11-05, `shared/cmake/SecretScanner.cmake` | Upstream BSD-3-Clause `LICENSE` |
| Abseil | Source candidate 20250814.1, `shared/cmake/SecretScanner.cmake` | Upstream Apache-2.0 `LICENSE` |
| cpp-httplib | 0.54.1, `shared/cmake/Http.cmake` | Upstream MIT `LICENSE` |
| MCP C++ SDK | Commit pinned in `shared/cmake/Mcp.cmake` | Upstream MIT `LICENSE` |
| toml++ | 3.4.0, `observer/cmake/ClientConfig.cmake` | Upstream MIT `LICENSE` |
| LibYAML | 0.2.5, `observer/cmake/ClientConfig.cmake` | Upstream MIT `License` |
| Dear ImGui | 1.93.0, `observer/tray/CMakeLists.txt` | Upstream MIT `LICENSE.txt` |
| FTXUI | 7.0.3, `shared/cmake/Ftxui.cmake` | Upstream MIT `LICENSE` |
| mimalloc | Optional Linux 3.5.3, `shared/cmake/CoreAllocator.cmake` | Upstream MIT `LICENSE` |

The vendored Tree-sitter source also includes its wasm libc license files.
Preserve them when redistributing those sources, even when that code is not
compiled by the native build.

OpenSSL Crypto and SQLite are selected through the
component build configuration. Record their actual versions and linkage for each
release. The macOS local kit copies its runtime Crypto dependency and its license;
other component packages may require provisioned system libraries. Do not assume
that one package's dependency inventory describes another package.

The macOS/Linux Sand terminal monitor links FTXUI statically; it needs no
curses/ncurses or terminfo runtime library. Native installations include FTXUI's
MIT notice at `share/shin-observer/licenses/ftxui/LICENSE`.

CMake installs applicable license texts below each component's `share` directory.
RE2 and Abseil are statically linked in the Core dev.19 source candidate;
their notices install under both component license directories. The published
dev.98 kit uses Core dev.18 and does not contain those new dependencies.
Package qualification checks the extracted payload, including notices and dynamic
library dependencies. This source inventory does not claim a vulnerability audit
or qualification of an unbuilt platform.
