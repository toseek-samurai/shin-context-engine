#include "evidence_expectations.h"
#include "context_internal.h"
#include "unicode_text.h"

namespace shin::context {

Json parse_evidence_expectations(const Json& object, const char* field,
                                const std::string& location, std::vector<std::string>& errors) {
    const auto value = object.find(field);
    if (value == object.end()) return Json::array();
    const auto fail = [&](const std::string& message) { errors.push_back(location + "." + field + message); };
    if (!value->is_array() || value->size() > 32) {
        fail(" must contain at most 32 evidence predicates.");
        return Json::array();
    }
    static const std::set<std::string> fields{"path", "heading", "contains", "status", "authorityLevel"};
    for (const auto& predicate : *value) {
        if (!predicate.is_object() || predicate.empty()) {
            fail(" requires nonempty predicate objects.");
            continue;
        }
        for (auto item = predicate.begin(); item != predicate.end(); ++item) {
            if (!fields.contains(item.key()) || !item->is_string() ||
                trim(item->get<std::string>()).empty() || item->get_ref<const std::string&>().size() > 4096) {
                fail(" has an unknown, empty or oversized field: " + item.key());
            } else if (item.key() == "path" && !profile_relative_path_valid(item->get<std::string>())) {
                fail(".path must be a portable project-relative path.");
            }
        }
    }
    return *value;
}

namespace {
std::string normalized(const std::string& text) { return unicode_casefold(compact_whitespace(text)); }
bool matches(const Json& record, const Json& predicate) {
    // Pack's display record is nested; its search-only fields are deliberately absent.
    const auto& row = record.contains("record") ? record.at("record") : record;
    for (auto field = predicate.begin(); field != predicate.end(); ++field) {
        const auto expected = field->get<std::string>();
        if (field.key() == "contains") {
            const auto text = json_string_value(row, "excerpt") + " " + json_string_value(row, "preview") +
                " " + json_string_value(row, "summary") + " " + json_string_value(row, "heading") +
                " " + json_string_value(row, "title");
            if (normalized(text).find(normalized(expected)) == std::string::npos) return false;
        } else {
            const auto key = field.key() == "authorityLevel" ? "authority_level" : field.key();
            const auto actual = field.key() == "heading"
                ? json_string_any(row, {"heading", "title"}) : json_string_value(row, key);
            if (field.key() == "path" ? actual != expected : normalized(actual) != normalized(expected)) return false;
        }
    }
    return true;
}
} // namespace

EvidenceAssessment assess_evidence(const Json& records, const Json& required, const Json& forbidden) {
    EvidenceAssessment result;
    const auto present = [&](const Json& predicate) {
        return std::any_of(records.begin(), records.end(), [&](const Json& row) { return matches(row, predicate); });
    };
    for (const auto& predicate : required) if (!present(predicate)) result.missing.push_back(predicate);
    for (const auto& predicate : forbidden) if (present(predicate)) result.forbidden.push_back(predicate);
    return result;
}

} // namespace shin::context
