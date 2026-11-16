#include "context_internal.h"
#include "generated_row_decoder.h"

#include <iostream>

using namespace shin::context;

namespace {
void expect(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

Json section() {
    return {{"document_id", "doc:fixture"}, {"path", "docs/context.md"},
        {"evidence_kind", "reference-document"}, {"verification_level", "declared"},
        {"heading", "Context"}, {"preview", "SAFE_PREVIEW"},
        {"indexed_text", "Context \xd0\xba\xd0\xbe\xd0\xbd\xd1\x82\xd0\xb5\xd0\xba\xd1\x81\xd1\x82 \xf0\x9f\x8c\x8d"},
        {"indexed_text_omitted_bytes", 0}};
}

void expect_section(const Json& row, bool escaped = false) {
    const auto decoded = decode_generated_section_row(row.dump(-1, ' ', escaped));
    expect(decoded.path == "docs/context.md" &&
           decoded.indexed_text == row.at("indexed_text").get_ref<const std::string&>(),
           "generated row changed source text or path");
}

void rejects_and_recovers(const std::string& input) {
    bool rejected = false;
    try { (void)decode_generated_section_row(input); }
    catch (const CliError& error) {
        expect(error.code() == "index-corrupt", "generated row returned the wrong refusal code");
        rejected = true;
    }
    expect(rejected, "malformed generated row was accepted");
    expect_section(section());
}

void malformed_rows() {
    const std::string valid = section().dump();
    for (const auto& input : {std::string("{}"), valid.substr(0, valid.size() - 2),
                             valid + " trailing", "[" + valid + "]"})
        rejects_and_recovers(input);
    for (const char* field : {"indexed_text", "indexed_text_omitted_bytes", "evidence_kind", "verification_level"}) {
        auto row = section(); row.erase(field);
        rejects_and_recovers(row.dump());
    }
    for (const auto& value : {Json(-1), Json(1.5), Json(true), Json("0"), Json(2147483648LL)}) {
        auto row = section(); row["indexed_text_omitted_bytes"] = value;
        rejects_and_recovers(row.dump());
    }
    for (const char* field : {"evidence_kind", "verification_level"}) {
        auto row = section(); row[field] = "unknown-fixture-kind";
        rejects_and_recovers(row.dump());
    }
    for (const auto& value : {Json(nullptr), Json(12), Json(std::string(kMaxSectionIndexedTextBytes + 1, 'x'))}) {
        auto row = section(); row["indexed_text"] = value;
        rejects_and_recovers(row.dump());
    }
    for (const std::string& bytes : {std::string("\xff", 1), std::string("\xc0\xaf", 2), std::string("\xed\xa0\x80", 3)}) {
        auto row = valid;
        row.replace(row.find("SAFE_PREVIEW"), std::string("SAFE_PREVIEW").size(), bytes);
        rejects_and_recovers(row);
    }
}

void source_fact_bounds() {
    const Json valid{{"id", "fact:fixture"}, {"path", "src/context.cpp"},
        {"evidence_kind", "reference-document"}, {"verification_level", "declared"},
        {"identifier_terms", Json::array({"context"})}, {"line", 1}, {"start_line", 1}, {"end_line", 1}};
    for (const auto& change : {Json{{"identifier_terms", Json::array({"context", "context"})}},
                              Json{{"identifier_terms", Json::array({12})}}, Json{{"line", 0}},
                              Json{{"start_line", 2}}, Json{{"end_line", 0}}}) {
        auto row = valid; row.update(change);
        bool rejected = false;
        try { (void)decode_generated_fact_row(row.dump()); }
        catch (const CliError& error) {
            expect(error.code() == "index-corrupt", "fact bounds returned the wrong refusal code");
            rejected = true;
        }
        expect(rejected, "invalid source fact bounds were accepted");
        expect(decode_generated_fact_row(valid.dump()).path == "src/context.cpp", "fact parser did not recover");
    }
}
}  // namespace

int main() {
    try {
        expect_section(section());
        expect_section(section(), true);
        malformed_rows();
        source_fact_bounds();
        auto large = section();
        large["ignored_payload"] = std::string(5U * 1024U * 1024U, 'x');
        for (int cycle = 0; cycle < 3; ++cycle) {
            expect_section(large);
            expect_section(section());
            expect_section(section(), true);
        }
        std::cout << "Generated JSONL: UTF-8, escapes, corrupt input, provenance, bounds and parser recovery passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
