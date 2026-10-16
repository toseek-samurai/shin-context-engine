#include "context_internal.h"

namespace shin::context {
namespace {

bool path_has_segment(const std::string& lower_path, std::string_view segment) {
    std::size_t start = 0;
    while (start <= lower_path.size()) {
        const std::size_t end = lower_path.find('/', start);
        const std::size_t length = end == std::string::npos ? lower_path.size() - start
                                                            : end - start;
        if (std::string_view(lower_path).substr(start, length) == segment) {
            return true;
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return false;
}

bool test_source_path(const std::string& path) {
    const std::string lower = to_lower(fs::path(path).lexically_normal().generic_string());
    if (path_has_segment(lower, "test") || path_has_segment(lower, "tests") ||
        path_has_segment(lower, "__tests__") || path_has_segment(lower, "testfixtures") ||
        path_has_segment(lower, "test-fixtures")) {
        return true;
    }
    const std::string lower_name = to_lower(basename_of(path));
    if (lower_name.find(".test.") != std::string::npos ||
        lower_name.find(".spec.") != std::string::npos ||
        lower_name.find("_test.") != std::string::npos ||
        lower_name.find("_tests.") != std::string::npos ||
        lower_name.find("-test.") != std::string::npos ||
        lower_name.find("-tests.") != std::string::npos) {
        return true;
    }
    const std::string name = basename_of(path);
    const std::size_t extension = name.find_last_of('.');
    const std::string stem = extension == std::string::npos ? name : name.substr(0, extension);
    return ends_with(stem, "Test") || ends_with(stem, "Tests");
}

bool one_of(std::string_view value, std::initializer_list<std::string_view> values) {
    return std::find(values.begin(), values.end(), value) != values.end();
}

bool decision_record(std::string_view doc_type, std::string_view record_type) {
    return one_of(record_type, {"decision", "adr", "decision-index"}) ||
           one_of(doc_type, {"adr", "decision-index"});
}

bool machine_contract(std::string_view doc_type, std::string_view record_type) {
    const std::initializer_list<std::string_view> values = {
        "contract", "schema", "manifest", "protocol", "openapi-contract",
        "asyncapi-contract"
    };
    return one_of(record_type, values) || one_of(doc_type, values);
}

bool operational_document(std::string_view doc_type, std::string_view record_type) {
    const std::initializer_list<std::string_view> values = {
        "runbook", "deployment", "operations", "security"
    };
    return one_of(record_type, values) || one_of(doc_type, values);
}

bool structured_evidence(std::string_view doc_type) {
    return one_of(doc_type, {"fact-store", "finding-store", "jsonl-artifact",
                             "json-artifact"});
}

}  // namespace

bool evidence_kind_valid(std::string_view value) noexcept {
    return one_of(value, {"implementation-source", "test-source", "machine-contract",
                          "decision-record", "architecture-document",
                          "operational-document", "generated-artifact",
                          "structured-evidence", "reference-document"});
}

bool verification_level_valid(std::string_view value) noexcept {
    return one_of(value, {"declared", "source-observed", "validated", "executed"});
}

EvidenceProvenance infer_record_evidence_provenance(
    const std::string& path,
    const std::string& doc_type,
    const std::string& record_type,
    const std::string& status,
    const std::string& authority_level,
    bool source_fact) {
    (void)status;
    (void)authority_level;

    EvidenceProvenance result;
    if (source_fact) {
        result.evidence_kind = test_source_path(path) ? "test-source"
                                                     : "implementation-source";
        result.verification_level = "source-observed";
        return result;
    }
    if (decision_record(doc_type, record_type)) {
        result.evidence_kind = "decision-record";
    } else if (machine_contract(doc_type, record_type)) {
        result.evidence_kind = "machine-contract";
    } else if (operational_document(doc_type, record_type)) {
        result.evidence_kind = "operational-document";
    } else if (doc_type == "architecture") {
        result.evidence_kind = "architecture-document";
    } else if (structured_evidence(doc_type)) {
        result.evidence_kind = "structured-evidence";
    } else {
        result.evidence_kind = "reference-document";
    }
    result.verification_level = "declared";
    return result;
}

EvidenceProvenance infer_document_evidence_provenance(const SourceDoc& doc) {
    const std::string lower_name = to_lower(basename_of(doc.path));
    if (doc.doc_type == "architecture" ||
        lower_name.find("architecture") != std::string::npos ||
        lower_name.find("boundary") != std::string::npos) {
        return {"architecture-document", "declared"};
    }
    if (operational_document(doc.doc_type, doc.doc_type) ||
        lower_name == "operations.md" || lower_name == "operations.json") {
        return {"operational-document", "declared"};
    }
    return infer_record_evidence_provenance(doc.path, doc.doc_type, doc.doc_type,
                                            doc.status, "", false);
}

}  // namespace shin::context
