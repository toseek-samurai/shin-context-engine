# Vendored Tree-sitter C++

Official release v0.23.4, commit f41e1a044c8a84ea9fa8577fdd2eab92ec96de02.
Source: https://github.com/tree-sitter/tree-sitter-cpp
License: MIT, retained in LICENSE. No build-time network or code generation.
Generated language ABI14 is within runtime0.27.0's supported ABI13-15 range.
Local patch: scanner factory uses `(void)` instead of `()` for an explicit C11
prototype, following the existing TS/TSX scanner policy. The table records the
unmodified upstream asset hashes; the patch is visible in the vendored source.

SHA256 pins:

| File | SHA256 |
| --- | --- |
| Official commit tar.gz | ad4b9b56602e9b14082637988f0042521006819960ed8c308bc24ded9ceb9331 |
| src/parser.c | 2a35a43b4af6c9f7b69624ac00c2c50808912591450dc79c05dea03ac1bae814 |
| src/scanner.c | cf60387d290271f4d2fb558d0569b2b9ef879cb72206a9df5cc73b0b1a4b20ff |
| src/tree_sitter/parser.h | a1f6ef161fbaf48a0e10fca90ef5290a062462b307b3898aa562993853b9f80a |
| src/tree_sitter/alloc.h | b29c1c9fb7cc82f58c84b376df1297d6e2737a1d655fd356db0859e3c29c2fea |
| src/tree_sitter/array.h | 4ff743903dc46f5db6aa54f31c6b4d160a8a9779e5b2ab1ee59ae7ebcd850ea1 |
| LICENSE | 2e0110e07abef7c2548b26ec9d6969775617ca539a0dc8dbeeb14d6452c711d1 |
