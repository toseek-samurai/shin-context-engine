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
| cpp-httplib | 0.54.1, `shared/cmake/Http.cmake` | Upstream MIT `LICENSE` |
| MCP C++ SDK | Commit pinned in `shared/cmake/Mcp.cmake` | Upstream MIT `LICENSE` |
| Dear ImGui | 1.92.9b, `observer/tray/CMakeLists.txt` | Upstream MIT `LICENSE.txt` |
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
Package qualification checks the extracted payload, including notices and dynamic
library dependencies. This source inventory does not claim a vulnerability audit
or qualification of an unbuilt platform.
