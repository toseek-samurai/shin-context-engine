#include "source_syntax.h"

#include <iostream>
#include <stdexcept>
#include <string>

using shin::context::chunk_source_syntax;

namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
void cover(const std::string& source, const std::string& extension, std::size_t budget) {
    const auto plan = chunk_source_syntax(source, extension, budget);
    require(plan.coverage_complete, "source coverage lost");
    std::string restored;
    for (const auto& part : plan.chunks) {
        require(part.begin == restored.size() && part.end > part.begin && part.end <= source.size(), "invalid partition");
        require(part.end - part.begin <= budget, "chunk budget exceeded");
        if (part.begin) require((static_cast<unsigned char>(source[part.begin]) & 0xc0) != 0x80, "split UTF8 code point");
        restored += source.substr(part.begin, part.end - part.begin);
    }
    require(restored == source, "source round trip differs");
}
}

int main() {
    try {
        for (const auto* extension : {".cpp", ".java", ".ts", ".tsx", ".sql"}) {
            for (const auto budget : {4U, 32U, 128U, 1024U})
                cover("// prefix\r\nclass A {\r\n int one() { return 1; }\r\n int two() { return 2; }\r\n}; // suffix\r\n", extension, budget);
        }
        const std::string units = "int a() { return 1; }\nint b() { return 2; }\nint c() { return 3; }\n";
        const auto parsed = chunk_source_syntax(units, ".cpp", 32);
        require(parsed.supported && parsed.syntax_complete && parsed.coverage_complete, "valid C++ not parsed");
        require(parsed.chunks.size() == 3, "fitting declarations split");
        for (const auto& part : parsed.chunks) require(!part.fallback, "fitting syntax fell back");
        cover(units, ".cpp", 32);
        const auto padded = std::string(100, ' ') + units + std::string(100, ' ');
        cover(padded, ".cpp", 32);
        const auto padded_plan = chunk_source_syntax(padded, ".cpp", 32);
        for (const auto* name : {"int a() { return 1; }", "int b() { return 2; }", "int c() { return 3; }"}) {
            bool found = false;
            for (const auto& part : padded_plan.chunks)
                found = found || padded.substr(part.begin, part.end - part.begin).find(name) != std::string::npos;
            require(found, "surrounding whitespace split fitting declaration");
        }
        std::string long_line = "export const text = \"";
        for (int i = 0; i < 2000; ++i) long_line += "\xd0\xb6\xf0\x9f\x8c\x90";
        long_line += "\";\n";
        cover(long_line, ".ts", 33);
        const auto long_plan = chunk_source_syntax(long_line, ".ts", 33);
        bool has_fallback = false;
        for (const auto& part : long_plan.chunks) has_fallback = has_fallback || part.fallback;
        require(has_fallback, "oversized leaf not marked fallback");
        const auto capped = chunk_source_syntax(units, ".cpp", 16, 1);
        require(!capped.coverage_complete && capped.chunks.size() == 1 && capped.chunks[0].end < units.size(), "cap hidden");
        const auto unsupported = chunk_source_syntax("select a from b;", ".sql", 8);
        require(!unsupported.supported && !unsupported.syntax_complete && unsupported.coverage_complete, "unsupported claims syntax");
        for (const auto& part : unsupported.chunks) require(part.fallback, "unsupported not marked fallback");
        const auto broken = chunk_source_syntax("int main( { ???", ".cpp", 8);
        require(broken.supported && !broken.syntax_complete && broken.coverage_complete, "broken syntax claims complete");
        cover("int main( { ???", ".cpp", 8);
        cover("", ".cpp", 8);
        cover("  \n\r\n\t", ".ts", 4);
        const auto large = chunk_source_syntax(std::string(1048577, 'a'), ".cpp");
        require(large.chunks.empty() && !large.coverage_complete, "file budget bypass");
        std::string nested = "int f() { return ";
        nested += std::string(100, '(') + "123456789" + std::string(100, ')') + "; }";
        cover(nested, ".cpp", 4);
        require(!chunk_source_syntax(nested, ".cpp", 4).syntax_complete, "depth cap hidden");
        for (const auto budget : {0U, 3U, 65537U}) {
            bool rejected = false;
            try { (void)chunk_source_syntax(units, ".cpp", budget); }
            catch (const std::invalid_argument&) { rejected = true; }
            require(rejected, "invalid byte budget admitted");
        }
        const auto repeat = chunk_source_syntax(units, ".cpp", 32);
        require(repeat.chunks.size() == parsed.chunks.size(), "nondeterministic chunk count");
        for (std::size_t i = 0; i < repeat.chunks.size(); ++i)
            require(repeat.chunks[i].begin == parsed.chunks[i].begin && repeat.chunks[i].end == parsed.chunks[i].end,
                    "nondeterministic chunk boundary");
        std::cout << "Structural chunking: language/budget matrix, exact coverage, UTF8, syntax, fallback and caps passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
