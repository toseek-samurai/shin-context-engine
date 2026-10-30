# Vendored Tree-sitter Objective-C++

MIT grammar from `packages/tree-sitter-objcpp` in
https://tangled.org/dunkirk.sh/dots at commit
`f744aa49e05971a2177f9b993f81a494744df7f5` (version 0.1.0).
It extends tree-sitter-cpp v0.23.4. Both authorship notices are retained in LICENSE.

The generated upstream parser exceeds the raw-file endpoint size limit. The
vendored parser was regenerated from the pinned, unmodified `src/grammar.json`
with official Tree-sitter CLI 0.25.10 and `--abi 15`. Runtime 0.27.0 supports ABI15.
UPSTREAM.json records input/output hashes and the scanner prototype patch.
Generation is a maintainer operation: builds need only C11, without Node, Rust,
Objective-C compiler, Cocoa headers, Xcode SDK, network access or code generation.

Scope: syntax observations rather than preprocessing, type checking or compiler
identity. Ordinary C++ files retain tree-sitter-cpp. Known upstream limitations
include structured bindings and lambda init-captures (the nested `[[` ambiguity),
and opaque macro bodies. Incomplete syntax remains explicit in Core diagnostics.
Headers included within an Objective-C implementation can be parsed in a bounded
implementation envelope; its synthetic coordinates and owner are removed before
source observation. No invented declaration is emitted for the envelope.
