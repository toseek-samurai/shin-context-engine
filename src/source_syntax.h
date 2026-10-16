#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace shin::context {

struct SyntaxDeclaration {
    std::string name;
    std::string kind;
    bool top_level = false;
    int line = 1;
    int start_line = 1;
    int end_line = 1;
    std::size_t byte_offset = 0;
    std::string header{};
    std::string qualified_name{};
};

struct SyntaxImport {
    std::string target;
    bool relative = false;
    int line = 1;
};

struct SyntaxFacts {
    bool supported = false;
    // False means parsing or bounded traversal did not observe a complete tree.
    bool complete = false;
    // Count observed during cursor traversal; it can be a lower bound when complete is false.
    std::size_t error_nodes = 0;
    std::string package_name;
    int package_line = 0;
    std::vector<SyntaxDeclaration> declarations;
    std::vector<SyntaxImport> imports;
};

// C++ syntax excludes .c; headers follow the existing C++ source classification.
SyntaxFacts inspect_source_syntax(std::string_view content, std::string_view extension);

struct SyntaxChunk {
    std::size_t begin = 0;
    std::size_t end = 0;
    bool fallback = false;
};

struct SyntaxChunking {
    bool supported = false;
    bool syntax_complete = false;
    bool coverage_complete = false;
    std::size_t visited_nodes = 0;
    std::vector<SyntaxChunk> chunks;
};

// Byte-exact, contiguous pieces; fallback and coverage are independent of syntax.
SyntaxChunking chunk_source_syntax(std::string_view content, std::string_view extension,
                                  std::size_t max_bytes = 1024, std::size_t max_chunks = 4096);

}  // namespace shin::context
