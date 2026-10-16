#include "context_internal.h"

namespace shin::context {

namespace {

enum class AuthoritySignal {
    None,
    History,
    Review,
    Advisory,
};

enum class LifecycleStatus {
    Unknown,
    Active,
    Accepted,
    Approved,
    Implemented,
    History,
    Review,
    Advisory,
};

bool status_token(const std::string& token, std::initializer_list<std::string_view> values) {
    return std::find(values.begin(), values.end(), token) != values.end();
}

AuthoritySignal authority_signal_from_tokens(const std::vector<std::string>& tokens) {
    bool history = false;
    bool review = false;
    bool advisory = false;
    for (const std::string& token : tokens) {
        history = history || status_token(token, {"superseded", "deprecated", "obsolete", "retired",
                                                   "archive", "archives", "history", "histories", "progress"});
        review = review || status_token(token, {"review", "reviews", "finding", "findings",
                                                  "recommendation", "recommendations"});
        advisory = advisory || status_token(token, {"rejected", "declined", "draft", "proposed",
                                                     "proposal", "proposals", "todo", "todos", "backlog",
                                                     "open", "advisory"});
    }
    if (history) {
        return AuthoritySignal::History;
    }
    if (review) {
        return AuthoritySignal::Review;
    }
    return advisory ? AuthoritySignal::Advisory : AuthoritySignal::None;
}

LifecycleStatus canonical_lifecycle_status(const std::string& status) {
    const std::string value = to_lower(compact_whitespace(status));
    if (value == "active") {
        return LifecycleStatus::Active;
    }
    if (value == "accepted") {
        return LifecycleStatus::Accepted;
    }
    if (value == "approved") {
        return LifecycleStatus::Approved;
    }
    if (value == "implemented") {
        return LifecycleStatus::Implemented;
    }
    if (value == "superseded" || value == "deprecated" || value == "obsolete" || value == "retired" ||
        value == "history" || value == "progress") {
        return LifecycleStatus::History;
    }
    if (value == "review" || value == "finding") {
        return LifecycleStatus::Review;
    }
    if (value == "rejected" || value == "declined" || value == "draft" || value == "proposed" ||
        value == "proposal" || value == "proposal-not-implemented" || value == "not-implemented" ||
        value == "todo" || value == "backlog" || value == "open" || value == "advisory" ||
        value == "recommendation") {
        return LifecycleStatus::Advisory;
    }
    return LifecycleStatus::Unknown;
}

struct MarkdownFence {
    char marker = '\0';
    std::size_t length = 0;
};

bool markdown_fence_marker(const std::string& line, MarkdownFence& fence) {
    const std::string trimmed = trim(line);
    if (trimmed.empty() || (trimmed.front() != '`' && trimmed.front() != '~')) {
        return false;
    }
    std::size_t length = 0;
    while (length < trimmed.size() && trimmed[length] == trimmed.front()) {
        ++length;
    }
    if (length < 3) {
        return false;
    }
    fence = {trimmed.front(), length};
    return true;
}

bool markdown_fence_closes(const std::string& line, const MarkdownFence& fence) {
    const std::string trimmed = trim(line);
    if (trimmed.empty() || trimmed.front() != fence.marker) {
        return false;
    }
    std::size_t length = 0;
    while (length < trimmed.size() && trimmed[length] == fence.marker) {
        ++length;
    }
    return length >= fence.length && trim(trimmed.substr(length)).empty();
}

std::string anchored_status_value(const std::string& line) {
    std::string value = trim(line);
    if (starts_with(value, "- ") || starts_with(value, "* ")) {
        value = trim(value.substr(2));
    }
    const std::string lower = to_lower(value);
    if (starts_with(lower, "status:")) {
        return trim(value.substr(7));
    }
    if (starts_with(lower, "**status:**")) {
        return trim(value.substr(11));
    }
    return "";
}

std::string explicit_status_in_range(const std::vector<std::string>& lines,
                                     std::size_t start,
                                     std::size_t end,
                                     int heading_boundary_level) {
    const std::size_t capped_end = std::min(end, lines.size());
    MarkdownFence fence;
    for (std::size_t index = std::min(start, capped_end); index < capped_end; ++index) {
        if (fence.marker != '\0') {
            if (markdown_fence_closes(lines[index], fence)) {
                fence = {};
            }
            continue;
        }
        MarkdownFence opening_fence;
        if (markdown_fence_marker(lines[index], opening_fence)) {
            fence = opening_fence;
            continue;
        }
        if (heading_boundary_level > 0 && index > start &&
            markdown_heading_level(lines[index]) >= heading_boundary_level) {
            break;
        }
        const std::string value = anchored_status_value(lines[index]);
        if (!value.empty()) {
            return truncate_text(compact_whitespace(value), 80);
        }
    }
    return "";
}

std::vector<std::string> path_status_tokens(const std::string& path) {
    std::vector<std::string> result;
    std::string component;
    const auto append_component_tokens = [&result](const std::string& raw_component) {
        std::string token;
        for (const unsigned char c : raw_component) {
            if (is_ascii_alnum(c)) {
                token.push_back(static_cast<char>(ascii_lower(c)));
                continue;
            }
            if (c == '-' || c == '_' || c == '.') {
                if (!token.empty()) {
                    result.push_back(std::move(token));
                    token.clear();
                }
                continue;
            }
            return;
        }
        if (!token.empty()) {
            result.push_back(std::move(token));
        }
    };

    for (const char c : path) {
        if (c == '/' || c == '\\') {
            append_component_tokens(component);
            component.clear();
        } else {
            component.push_back(c);
        }
    }
    append_component_tokens(component);
    return result;
}

AuthoritySignal authority_signal_from_status(const std::string& status) {
    switch (canonical_lifecycle_status(status)) {
        case LifecycleStatus::History:
            return AuthoritySignal::History;
        case LifecycleStatus::Review:
            return AuthoritySignal::Review;
        case LifecycleStatus::Advisory:
            return AuthoritySignal::Advisory;
        case LifecycleStatus::Unknown:
        case LifecycleStatus::Active:
        case LifecycleStatus::Accepted:
        case LifecycleStatus::Approved:
        case LifecycleStatus::Implemented:
            return AuthoritySignal::None;
    }
    return AuthoritySignal::None;
}

bool decision_status_is_accepted(const std::string& status) {
    switch (canonical_lifecycle_status(status)) {
        case LifecycleStatus::Active:
        case LifecycleStatus::Accepted:
        case LifecycleStatus::Approved:
        case LifecycleStatus::Implemented:
            return true;
        case LifecycleStatus::Unknown:
        case LifecycleStatus::History:
        case LifecycleStatus::Review:
        case LifecycleStatus::Advisory:
            return false;
    }
    return false;
}

bool source_of_truth_type(const std::string& doc_type, const std::string& record_type) {
    const std::string lower_doc_type = to_lower(doc_type);
    const std::string lower_record_type = to_lower(record_type);
    if (lower_record_type == "contract" || lower_record_type == "runbook") {
        return true;
    }
    return lower_doc_type == "contract" || lower_doc_type == "openapi-contract" ||
           lower_doc_type == "asyncapi-contract" || lower_doc_type == "schema" ||
           lower_doc_type == "manifest" || lower_doc_type == "runbook" ||
           lower_doc_type == "deployment" || lower_doc_type == "operations" ||
           lower_doc_type == "security";
}

}  // namespace

std::string explicit_metadata_status(const Json* json_doc) {
    if (json_doc == nullptr) {
        return "";
    }
    for (const char* key : {"status", "state"}) {
        const std::string value = json_string_value(*json_doc, key);
        if (!value.empty()) {
            return truncate_text(compact_whitespace(value), 80);
        }
    }
    return "";
}

std::string explicit_document_status(const std::string& content) {
    const std::vector<std::string> lines = split_lines(content);
    return explicit_status_in_range(lines, 0, lines.size(), 2);
}

std::vector<int> unfenced_markdown_heading_levels(const std::vector<std::string>& lines) {
    std::vector<int> levels(lines.size(), 0);
    MarkdownFence fence;
    for (std::size_t index = 0; index < lines.size(); ++index) {
        if (fence.marker != '\0') {
            if (markdown_fence_closes(lines[index], fence)) {
                fence = {};
            }
            continue;
        }
        MarkdownFence opening_fence;
        if (markdown_fence_marker(lines[index], opening_fence)) {
            fence = opening_fence;
            continue;
        }
        levels[index] = markdown_heading_level(lines[index]);
    }
    return levels;
}

std::string explicit_section_status(const std::vector<std::string>& lines,
                                    std::size_t start,
                                    std::size_t end) {
    return explicit_status_in_range(lines, start, end, 1);
}

std::string fallback_status_from_path(const std::string& path) {
    const std::vector<std::string> tokens = path_status_tokens(path);
    bool open = false;
    bool backlog = false;
    bool draft = false;
    bool rejected = false;
    for (const std::string& token : tokens) {
        rejected = rejected || status_token(token, {"rejected", "declined"});
        open = open || token == "open";
        backlog = backlog || status_token(token, {"todo", "todos", "backlog"});
        draft = draft || status_token(token, {"draft", "proposed", "proposal", "proposals"});
    }
    const AuthoritySignal path_signal = authority_signal_from_tokens(tokens);
    if (path_signal == AuthoritySignal::History) {
        return "superseded";
    }
    if (path_signal == AuthoritySignal::Review) {
        return "review";
    }
    if (rejected) {
        return "rejected";
    }
    if (open) {
        return "open";
    }
    if (backlog) {
        return "backlog";
    }
    if (draft) {
        return "draft";
    }
    return path_signal == AuthoritySignal::Advisory ? "draft" : "active";
}

std::string infer_authority_level(const std::string& doc_type,
                                  const std::string& record_type,
                                  const std::string& path,
                                  const std::string& status) {
    const std::string effective_status = status.empty() ? fallback_status_from_path(path) : status;
    switch (authority_signal_from_tokens(path_status_tokens(path))) {
        case AuthoritySignal::History:
            return "history";
        case AuthoritySignal::Review:
            return "review";
        case AuthoritySignal::Advisory:
            return "advisory";
        case AuthoritySignal::None:
            break;
    }
    switch (authority_signal_from_status(effective_status)) {
        case AuthoritySignal::History:
            return "history";
        case AuthoritySignal::Review:
            return "review";
        case AuthoritySignal::Advisory:
            return "advisory";
        case AuthoritySignal::None:
            break;
    }
    if (to_lower(record_type) == "decision") {
        return decision_status_is_accepted(effective_status) ? "accepted" : "advisory";
    }
    if (source_of_truth_type(doc_type, record_type)) {
        return "source-of-truth";
    }
    return "advisory";
}

std::string fact_authority_level_or_default(const SourceDoc& doc,
                                            const std::string& record_type,
                                            const std::string& status,
                                            const std::string& explicit_level) {
    if (!explicit_level.empty()) {
        return explicit_level;
    }
    return infer_authority_level(doc.doc_type, record_type, doc.path, status);
}

}  // namespace shin::context
