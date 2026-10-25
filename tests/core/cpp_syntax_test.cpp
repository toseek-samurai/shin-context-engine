#include "source_syntax.h"

#include <algorithm>
#include <iostream>
#include <stdexcept>

using shin::context::inspect_source_syntax;
using shin::context::SyntaxFacts;

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

bool has(const SyntaxFacts& facts, const char* name, const char* kind = "function") {
    return std::any_of(facts.declarations.begin(), facts.declarations.end(), [&](const auto& row) {
        return row.name == name && row.kind == kind;
    });
}

void declarations() {
    const auto facts = inspect_source_syntax(R"cpp(namespace outer {
class Sample {
public:
  Sample(int value) : member_(value) {}
  ~Sample() = default;
  int inlineCall(int value) const { return value + member_; }
  int overloaded(); int overloaded(int value);
  int operator()(int value) const { return value; }
  explicit operator bool() const { return true; }
  int member_;
};
int Sample::overloaded() { return 1; }
template<class T>
[[nodiscard]] auto multiline(
    T value) noexcept -> T {
  return value;
}
struct Record { int value; };
enum class Kind { first, second };
}
)cpp", ".cpp");
    require(facts.supported && facts.complete, "C++ grammar did not parse declarations");
    for (const auto* name : {"Sample", "~Sample", "inlineCall", "overloaded", "operator()", "operator bool", "multiline"})
        require(has(facts, name), name);
    require(has(facts, "Sample", "class") && has(facts, "Record", "struct") &&
            has(facts, "Kind", "enum") && has(facts, "outer", "namespace"), "missing named owner");
    require(!has(facts, "member_"), "initializer mistaken for function");
    const auto count = std::count_if(facts.declarations.begin(), facts.declarations.end(), [](const auto& row) {
        return row.name == "overloaded";
    });
    require(count == 3, "overloads or declaration/definition collapsed");
    const auto multi = std::find_if(facts.declarations.begin(), facts.declarations.end(), [](const auto& row) {
        return row.name == "multiline";
    });
    require(multi != facts.declarations.end() && multi->start_line == 13 && multi->end_line == 17 &&
            multi->qualified_name == "outer::multiline" && multi->header.find("noexcept") != std::string::npos,
            "template header, ownership or multiline range lost");
    const auto method = std::find_if(facts.declarations.begin(), facts.declarations.end(), [](const auto& row) {
        return row.name == "inlineCall";
    });
    require(method->qualified_name == "outer::Sample::inlineCall", "inline method owner lost");
}

void false_positives() {
    const auto facts = inspect_source_syntax(R"cpp(struct Forward;
void consume(const struct stat& info) { invoke(info); }
int (*callback)(int);
int *pointerResult(int value);
int (*factory())(int);
int &referenceResult();
void withCallback(void (*handler)(int));
using Function = void(int);
auto closure = [](int value) { return value; };
const char *text = "void invented() { }";
// int commented() { return 1; }
)cpp", ".cpp");
    require(facts.complete, "valid C++ fixture incomplete");
    for (const auto* name : {"consume", "pointerResult", "factory", "referenceResult", "withCallback"})
        require(has(facts, name), name);
    for (const auto* name : {"callback", "handler", "invoke", "invented", "commented", "closure"})
        require(!has(facts, name), name);
    require(!has(facts, "stat", "struct") && !has(facts, "Forward", "struct"), "type use claimed as definition");
}

void recovery_and_bounds() {
    const auto broken = inspect_source_syntax("int good() { return 1; }\nint broken( {\n", ".cpp");
    require(broken.supported && !broken.complete && has(broken, "good") && !has(broken, "broken"),
            "malformed declaration gained a syntax range");
    const auto large = inspect_source_syntax(std::string(1024 * 1024 + 1, ' '), ".cpp");
    require(large.supported && !large.complete && large.declarations.empty(), "source cap not retained");
    const auto macros = inspect_source_syntax("#if FIRST\nint first() {}\n#else\nint second() {}\n#endif\n", ".cpp");
    require(macros.complete && has(macros, "first") && has(macros, "second"), "conditional source observations lost");
    require(!inspect_source_syntax("int plain() {}", ".c").supported, "C route changed");
    std::string many;
    for (int i = 0; i < 50000; ++i) many += "int value;\n";
    const auto capped = inspect_source_syntax(many, ".cpp");
    require(capped.supported && !capped.complete, "bounded traversal claimed completeness");
    for (const auto* extension : {".h", ".hh", ".hpp", ".hxx", ".cc", ".cxx", ".inl", ".ipp"})
        require(has(inspect_source_syntax("int headerCall();", extension), "headerCall"), extension);
}

void conversion_operators() {
    const auto facts = inspect_source_syntax(
        "struct Conversion { operator const char*() const; };\n"
        "Conversion::operator const char*() const { return \"x\"; }\n", ".cpp");
    require(facts.complete, "conversion operators did not parse");
    require(std::count_if(facts.declarations.begin(), facts.declarations.end(), [](const auto& row) {
        return row.name == "operator const char*";
    }) == 2, "qualified conversion declaration/definition lost");
}
} // namespace

int main() {
    try {
        declarations();
        false_positives();
        recovery_and_bounds();
        conversion_operators();
        std::cout << "C++ syntax tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
