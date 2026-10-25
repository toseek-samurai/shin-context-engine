#include "generated_row_decoder.h"

#include "context_internal.h"
#include "simdjson/simdjson.h"

namespace shin::context {
namespace {

constexpr std::size_t kMaximumRetainedParserCapacity = 4U * 1024U * 1024U;

class GeneratedRowDocument {
public:
    GeneratedRowDocument(std::string_view line, std::string_view label)
        : input_(line) {
        if (line.size() > kMaximumRetainedParserCapacity) {
            oversized_parser_ = std::make_unique<simdjson::dom::parser>();
        }
        simdjson::error_code error = parser().parse(input_).get(document_);
        if (!error) {
            error = document_.get(object_);
        }
        if (error) {
            throw_index_error(
                "index-corrupt",
                "Generated index row is not a valid JSON object in " +
                    std::string(label) + ": " + simdjson::error_message(error));
        }
    }

    const simdjson::dom::object& object() const noexcept { return object_; }

private:
    simdjson::dom::parser& parser() {
        if (oversized_parser_) {
            return *oversized_parser_;
        }
        thread_local simdjson::dom::parser retained_parser;
        return retained_parser;
    }

    simdjson::padded_string input_;
    std::unique_ptr<simdjson::dom::parser> oversized_parser_;
    simdjson::dom::element document_;
    simdjson::dom::object object_;
};

std::optional<simdjson::dom::element> row_field(const simdjson::dom::object& object,
                                                 std::string_view key) {
    simdjson::dom::element value;
    if (object.at_key(key).get(value)) {
        return std::nullopt;
    }
    return value;
}

std::string row_string(const simdjson::dom::object& object, std::string_view key) {
    const std::optional<simdjson::dom::element> value = row_field(object, key);
    std::string_view text;
    if (!value.has_value() || value->get(text)) {
        return {};
    }
    return std::string(text);
}

std::string optional_source_string(const simdjson::dom::object& object, std::string_view key) {
    const auto value = row_field(object, key);
    if (!value) return {};
    std::string_view text;
    if (value->get(text)) {
        throw_index_error("index-corrupt", "Generated source span field must be a string: " + std::string(key));
    }
    return std::string(text);
}

template <typename Validator>
std::string required_enum_string(const simdjson::dom::object& object,
                                 std::string_view key,
                                 std::string_view store,
                                 Validator&& validator) {
    const std::optional<simdjson::dom::element> value = row_field(object, key);
    std::string_view text;
    if (!value.has_value() || value->get(text) || !validator(text)) {
        throw_index_error(
            "index-corrupt",
            "Generated " + std::string(store) + " row has a missing, non-string, or unknown " +
                std::string(key) + ".");
    }
    return std::string(text);
}

std::string required_evidence_kind(const simdjson::dom::object& object,
                                   std::string_view store) {
    return required_enum_string(object, "evidence_kind", store, evidence_kind_valid);
}

std::string required_verification_level(const simdjson::dom::object& object,
                                        std::string_view store) {
    return required_enum_string(object, "verification_level", store,
                                verification_level_valid);
}

int row_integer(const simdjson::dom::object& object,
                std::string_view key,
                int fallback) {
    const std::optional<simdjson::dom::element> value = row_field(object, key);
    if (!value.has_value()) {
        return fallback;
    }
    std::int64_t signed_value = 0;
    if (!value->get(signed_value)) {
        if (signed_value >= std::numeric_limits<int>::min() &&
            signed_value <= std::numeric_limits<int>::max()) {
            return static_cast<int>(signed_value);
        }
        return fallback;
    }
    std::uint64_t unsigned_value = 0;
    if (!value->get(unsigned_value) &&
        unsigned_value <= static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
        return static_cast<int>(unsigned_value);
    }
    return fallback;
}

std::vector<std::string> row_string_array(const simdjson::dom::object& object,
                                          std::string_view key) {
    const std::optional<simdjson::dom::element> value = row_field(object, key);
    simdjson::dom::array array;
    if (!value.has_value() || value->get(array)) {
        return {};
    }
    std::vector<std::string> strings;
    for (simdjson::dom::element item : array) {
        std::string_view text;
        if (!item.get(text)) {
            strings.emplace_back(text);
        }
    }
    return strings;
}

std::vector<std::string> required_string_array(const simdjson::dom::object& object,
                                               std::string_view key,
                                               std::string_view store) {
    const std::optional<simdjson::dom::element> value = row_field(object, key);
    simdjson::dom::array array;
    if (!value.has_value() || value->get(array)) {
        throw_index_error("index-corrupt",
                          "Generated " + std::string(store) + " row has a missing or non-array " +
                              std::string(key) + ".");
    }
    std::vector<std::string> strings;
    for (simdjson::dom::element item : array) {
        std::string_view text;
        if (item.get(text)) {
            throw_index_error("index-corrupt",
                              "Generated " + std::string(store) + " row has a non-string " +
                                  std::string(key) + " entry.");
        }
        strings.emplace_back(text);
    }
    return strings;
}

FactRow decode_common_fact(const simdjson::dom::object& object, std::string_view store) {
    FactRow row;
    row.id = row_string(object, "id");
    row.path = row_string(object, "path");
    row.status = row_string(object, "status");
    row.area = row_string(object, "area");
    row.title = row_string(object, "title");
    row.summary = row_string(object, "summary");
    row.authority_level = row_string(object, "authority_level");
    row.evidence_kind = required_evidence_kind(object, store);
    row.verification_level = required_verification_level(object, store);
    row.topics = row_string_array(object, "topics");
    row.line = row_integer(object, "line", 1);
    row.start_line = row_integer(object, "start_line", row.line);
    row.end_line = row_integer(object, "end_line", row.start_line);
    if (row.line < 1 || row.start_line < 1 || row.end_line < row.start_line ||
        row.line < row.start_line || row.line > row.end_line) {
        throw_index_error("index-corrupt",
                          "Generated " + std::string(store) + " row has an invalid source range.");
    }
    row.identifier_terms = required_string_array(object, "identifier_terms", store);
    row.symbol_name = optional_source_string(object, "symbol_name");
    row.span_hash = optional_source_string(object, "span_hash");
    row.range_origin = optional_source_string(object, "range_origin");
    if ((!row.span_hash.empty() &&
         (row.span_hash.size() != 24 || !row.span_hash.starts_with("fnv1a64:") ||
          !std::all_of(row.span_hash.begin() + 8, row.span_hash.end(), [](unsigned char c) {
              return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
          }))) ||
        (!row.range_origin.empty() && row.range_origin != "syntax" && row.range_origin != "syntax-header" &&
         row.range_origin != "lexical" && row.range_origin != "lexical-window" && row.range_origin != "payload-file")) {
        throw_index_error("index-corrupt", "Generated source span provenance is invalid.");
    }
    if (row.identifier_terms.size() > 256) {
        throw_index_error("index-corrupt",
                          "Generated " + std::string(store) +
                              " row exceeds the identifier term limit.");
    }
    std::set<std::string> unique_identifiers;
    for (const std::string& identifier : row.identifier_terms) {
        if (identifier.empty() || identifier.size() > 128 ||
            !unique_identifiers.insert(identifier).second) {
            throw_index_error("index-corrupt",
                              "Generated " + std::string(store) +
                                  " row has an invalid or duplicate identifier term.");
        }
    }
    return row;
}

}  // namespace

SectionRow decode_generated_section_row(std::string_view line) {
    const GeneratedRowDocument document(line, "sections.jsonl");
    const simdjson::dom::object& object = document.object();
    SectionRow row;
    row.document_id = row_string(object, "document_id");
    row.path = row_string(object, "path");
    row.status = row_string(object, "status");
    row.authority_level = row_string(object, "authority_level");
    row.evidence_kind = required_evidence_kind(object, "sections.jsonl");
    row.verification_level = required_verification_level(object, "sections.jsonl");
    row.heading = row_string(object, "heading");
    row.anchor = row_string(object, "anchor");
    row.preview = row_string(object, "preview");
    row.indexed_text = required_enum_string(object, "indexed_text", "section",
        [](std::string_view) { return true; });
    row.indexed_text_omitted_bytes = row_integer(object, "indexed_text_omitted_bytes", -1);
    if (row.indexed_text.size() > kMaxSectionIndexedTextBytes || row.indexed_text_omitted_bytes < 0) {
        throw_index_error("index-corrupt", "Invalid indexed section text bounds.");
    }
    row.topics = row_string_array(object, "topics");
    row.line = row_integer(object, "line", 1);
    row.level = row_integer(object, "level", 1);
    row.start_line = row_integer(object, "start_line", row.line);
    row.end_line = row_integer(object, "end_line", row.start_line);
    row.chunk_index = row_integer(object, "chunk_index", 0);
    row.row_type = row_string(object, "row_type");
    if (row.row_type.empty()) {
        row.row_type = "section";
    }
    return row;
}

SourceDoc decode_generated_document_row(std::string_view line) {
    const GeneratedRowDocument document(line, "documents.jsonl");
    const simdjson::dom::object& object = document.object();
    SourceDoc row;
    row.id = row_string(object, "id");
    row.path = row_string(object, "path");
    row.kind = row_string(object, "kind");
    row.doc_type = row_string(object, "doc_type");
    row.area = row_string(object, "area");
    row.status = row_string(object, "status");
    row.evidence_kind = required_evidence_kind(object, "documents.jsonl");
    row.verification_level = required_verification_level(object, "documents.jsonl");
    row.title = row_string(object, "title");
    row.summary = row_string(object, "summary");
    row.content_hash = row_string(object, "content_hash");
    row.topics = row_string_array(object, "topics");
    row.byte_count = row_integer(object, "bytes", 0);
    row.line_count = row_integer(object, "line_count", 0);
    row.heading_count = row_integer(object, "heading_count", 0);
    return row;
}

LinkRow decode_generated_link_row(std::string_view line) {
    const GeneratedRowDocument document(line, "links.jsonl");
    const simdjson::dom::object& object = document.object();
    LinkRow row;
    row.path = row_string(object, "path");
    row.kind = row_string(object, "kind");
    row.text = row_string(object, "text");
    row.target = row_string(object, "target");
    row.status = row_string(object, "status");
    row.resolved_path = row_string(object, "resolved_path");
    row.line = row_integer(object, "line", 1);
    if (row_field(object, "target_start_line") || row_field(object, "target_end_line") ||
        row_field(object, "target_range_origin")) {
        row.target_start_line = row_integer(object, "target_start_line", 0);
        row.target_end_line = row_integer(object, "target_end_line", 0);
        row.target_range_origin = optional_source_string(object, "target_range_origin");
        if (row.target_start_line < 1 || row.target_end_line < row.target_start_line ||
            (row.target_range_origin != "rsx-opening-tag" && row.target_range_origin != "syntax-declaration") ||
            row.status != "resolved" || row.resolved_path.empty() ||
            (row.kind != "source-reference" && row.kind != "source-import")) {
            throw_index_error("index-corrupt", "Generated source relation has an invalid target range.");
        }
    }
    return row;
}

FactRow decode_generated_fact_row(std::string_view line) {
    const GeneratedRowDocument document(line, "facts.jsonl");
    const simdjson::dom::object& object = document.object();
    FactRow row = decode_common_fact(object, "facts.jsonl");
    row.record_type = row_string(object, "record_type");
    row.source = row_string(object, "source");
    return row;
}

FactRow decode_generated_decision_row(std::string_view line) {
    const GeneratedRowDocument document(line, "decisions.jsonl");
    FactRow row = decode_common_fact(document.object(), "decisions.jsonl");
    row.record_type = "decision";
    row.source = row.path;
    return row;
}

FactRow decode_generated_typed_reference_row(std::string_view line) {
    const GeneratedRowDocument document(line, "typed-reference.jsonl");
    const simdjson::dom::object& object = document.object();
    FactRow row = decode_common_fact(object, "typed-reference.jsonl");
    row.record_type = row_string(object, "type");
    row.source = row.path;
    return row;
}

std::string generated_row_decoder_version() {
    return SIMDJSON_VERSION;
}

std::string generated_row_decoder_implementation() {
    const simdjson::implementation* implementation = simdjson::get_active_implementation();
    return implementation == nullptr ? "unavailable" : std::string(implementation->name());
}

}  // namespace shin::context
