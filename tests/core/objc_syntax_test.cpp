#include "source_syntax.h"

#include <algorithm>
#include <iostream>
#include <stdexcept>

using shin::context::inspect_source_syntax;
using shin::context::SyntaxFacts;

namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
bool has(const SyntaxFacts& facts, const char* name, const char* kind) {
    return std::any_of(facts.declarations.begin(), facts.declarations.end(), [&](const auto& row) {
        return row.name == name && row.kind == kind;
    });
}
void mixed_source() {
    const std::string source = R"objc(#import "Local.h"
#import <Cocoa/Cocoa.h>
@protocol Presenting
@property(readonly) int count;
- (void)show:(id)sender title:(id)title;
@end
@interface Monitor : NSObject <Presenting>
@property(copy) void (^completion)(int);
+ (id)shared;
@end
@interface Monitor (Updates)
- (void)check;
@end
@interface Monitor ()
@property int phase;
@end
@implementation Monitor
- (void)show:(id)sender title:(id)title {
  auto adjusted = [](int n) { return n + 1; };
  [self render:sender];
  const char* text = "@interface Invented @end";
  (void)adjusted; (void)text;
}
+ (id)shared { return nil; }
@end
@implementation Monitor (Updates)
- (void)check { [self show:nil title:@"Ready"]; }
@end
namespace native { template<class T> T scale(T value) { return value + value; } }
const char* raw = R"raw(@interface Fiction @end)raw";
// - (void)commented {}
)objc";
    const auto facts = inspect_source_syntax(source, ".mm");
    require(facts.supported && facts.complete && facts.language == "objcpp", "mixed Objective-C++ not parsed");
    for (const auto* kind : {"interface", "implementation"}) require(has(facts, "Monitor", kind), kind);
    require(has(facts, "Presenting", "protocol") && has(facts, "Monitor(Updates)", "category") &&
            has(facts, "Monitor()", "extension"), "Objective-C owners lost");
    require(has(facts, "count", "property") && has(facts, "completion", "property") &&
            has(facts, "phase", "property"), "property declarator lost");
    require(has(facts, "show:title:", "method") && has(facts, "shared", "method") &&
            has(facts, "scale", "function"), "selector or C++ function lost");
    for (const auto* name : {"render:", "Invented", "Fiction", "commented", "adjusted"})
        require(!std::any_of(facts.declarations.begin(), facts.declarations.end(), [&](const auto& d) {
            return d.name == name;
        }), "expression, comment or literal promoted into declaration");
    const auto implementation = std::find_if(facts.declarations.begin(), facts.declarations.end(), [](const auto& d) {
        return d.name == "show:title:" && d.start_line == 18;
    });
    require(implementation != facts.declarations.end() && implementation->end_line == 23 &&
            implementation->qualified_name == "Monitor::-show:title:" &&
            implementation->header.find("adjusted") == std::string::npos, "method range, scope or header lost");
    require(facts.imports.size() == 1 && facts.imports.front().target == "Local.h" &&
            facts.imports.front().relative, "SDK import treated as a project file");
    const auto objc = inspect_source_syntax("@implementation Sample\n- (void)run {}\n@end\n", ".m");
    require(objc.complete && objc.language == "objc" && has(objc, "run", "method"), "Objective-C route missing");
}
void headers_and_identity() {
    const auto cocoa = inspect_source_syntax("@interface Header : NSObject\n- (void)open;\n@end\n", ".h");
    require(cocoa.complete && cocoa.language == "objcpp" && has(cocoa, "open", "method"), "Cocoa header not detected");
    for (const auto* declaration : {"@class FuturePanel;\n", "@protocol FutureProtocol;\n"}) {
        const auto forward = inspect_source_syntax(declaration, ".h");
        require(forward.complete && forward.language == "objcpp" && forward.declarations.empty(),
                "forward declarations misclassified or promoted into definitions");
    }
    const auto cpp = inspect_source_syntax("auto [one, two] = pair;\nint ordinary();\n", ".h");
    require(cpp.complete && cpp.language == "cpp" && has(cpp, "ordinary", "function"), "ordinary C++ header route changed");
    const std::string fragment = "// implementation fragment\r\n- (void)open:(id)sender {\r\n (void)sender;\r\n}\r\n+ (void)open:(id)sender {}";
    const auto methods = inspect_source_syntax(fragment, ".h");
    require(methods.complete && methods.language == "objcpp" && methods.declarations.size() == 2,
            "standalone implementation header not observed");
    require(methods.declarations[0].name == "open:" && methods.declarations[0].start_line == 2 &&
            methods.declarations[0].end_line == 4 && methods.declarations[0].byte_offset == fragment.find("open:"),
            "fragment coordinates shifted");
    require(methods.declarations[0].qualified_name == "-open:" && methods.declarations[1].qualified_name == "+open:",
            "synthetic owner leaked or class/instance distinction lost");
    const auto identity = inspect_source_syntax("@interface A\n- (void)same; + (void)same;\n@end\n"
        "@implementation A\n- (void)same {} + (void)same {}\n@end\n", ".mm");
    require(identity.complete, "same-line methods not parsed");
    std::vector<std::size_t> offsets;
    for (const auto& row : identity.declarations) if (row.name == "same") offsets.push_back(row.byte_offset);
    std::sort(offsets.begin(), offsets.end());
    require(offsets.size() == 4 && std::adjacent_find(offsets.begin(), offsets.end()) == offsets.end(),
            "method declarations and definitions collapsed");
    for (const auto budget : {4U, 32U, 128U}) {
        const auto chunks = shin::context::chunk_source_syntax(fragment, ".h", budget);
        require(chunks.syntax_complete && chunks.coverage_complete, "fragment chunk coverage lost");
        std::size_t cursor = 0;
        for (const auto& chunk : chunks.chunks) {
            require(chunk.begin == cursor && chunk.end > chunk.begin && chunk.end <= fragment.size() &&
                    chunk.end - chunk.begin <= budget, "fragment chunk bounds shifted");
            cursor = chunk.end;
        }
        require(cursor == fragment.size(), "fragment chunk suffix lost");
    }
}
void partial_and_bounded() {
    const auto broken = inspect_source_syntax("@implementation A\n- (void)good {}\n- (void)bad:( {\n@end\n", ".mm");
    require(broken.supported && !broken.complete && has(broken, "good", "method") && !has(broken, "bad:", "method"),
            "malformed method gained a complete range");
    const auto unsupported_cpp = inspect_source_syntax("auto [one, two] = pair;", ".mm");
    require(!unsupported_cpp.complete, "unsupported Objective-C++ grammar construct hidden");
    const auto large = inspect_source_syntax(std::string(1024 * 1024 + 1, ' '), ".mm");
    require(large.supported && !large.complete && large.declarations.empty(), "source cap bypassed");
    std::string many;
    for (int i = 0; i < 50000; ++i) many += "int value;\n";
    require(!inspect_source_syntax(many, ".mm").complete, "node cap bypassed");
    require(!inspect_source_syntax("int broken( {", ".h").complete, "broken C++ header relabeled as complete");
}
}

int main() {
    try { mixed_source(); headers_and_identity(); partial_and_bounded(); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    return 0;
}
