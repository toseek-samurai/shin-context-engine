#pragma once
#include <nlohmann/json.hpp>
#include "shin/observer_protocol.h"
#include <algorithm>
#include <string>
#include <vector>

namespace shin::observer {
// Only bounded identifiers and safe project-relative expected paths cross the
// diagnostic boundary; query text, source content, and absolute paths do not.
inline std::string validation_identifier(const nlohmann::json& value) {
    if (!value.is_string()) return {};
    const auto& text = value.get_ref<const std::string&>();
    if (text.empty() || text.size() > 96 || !std::all_of(text.begin(), text.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
    })) return {};
    return text;
}

// Expected paths are useful to a project owner, but must never become an
// arbitrary path disclosure. Accept only short, normalized project-relative
// paths with ordinary printable components.
inline std::string validation_expected_path(const nlohmann::json& value) {
    if (!value.is_string()) return {};
    const auto& path = value.get_ref<const std::string&>();
    if (path.empty() || path.size() > 160 || path.front() == '/' || path.back() == '/' ||
        path.find('\\') != std::string::npos || path.find(':') != std::string::npos) return {};
    std::size_t component_start = 0;
    for (std::size_t i = 0; i <= path.size(); ++i) {
        if (i != path.size() && path[i] != '/') {
            const auto c = static_cast<unsigned char>(path[i]);
            if (c < 32 || c == 127) return {};
            continue;
        }
        const auto length = i - component_start;
        if (length == 0 || (length == 1 && path[component_start] == '.') ||
            (length == 2 && path[component_start] == '.' && path[component_start + 1] == '.')) return {};
        component_start = i + 1;
    }
    return path;
}

inline void append_validation_paths(nlohmann::json& entry, const nlohmann::json& paths,
                                    bool& truncated) {
    using J = nlohmann::json;
    if (!paths.is_array()) return;
    for (const auto& value : paths) {
        const auto path = validation_expected_path(value);
        if (path.empty()) {
            if (value.is_string()) truncated = true;
            continue;
        }
        if (!entry.contains("expected_paths")) entry["expected_paths"] = J::array();
        if (std::find(entry["expected_paths"].begin(), entry["expected_paths"].end(), path) !=
            entry["expected_paths"].end()) continue;
        if (entry["expected_paths"].size() == 2) { truncated = true; break; }
        entry["expected_paths"].push_back(path);
    }
}

inline bool drop_last_validation_path(nlohmann::json& diagnostic) {
    for (auto gate = diagnostic["gates"].rbegin(); gate != diagnostic["gates"].rend(); ++gate) {
        for (auto item = gate->at("cases").rbegin(); item != gate->at("cases").rend(); ++item) {
            const auto paths = item->find("expected_paths");
            if (paths == item->end() || !paths->is_array() || paths->empty()) continue;
            paths->erase(paths->end() - 1);
            if (paths->empty()) item->erase("expected_paths");
            return true;
        }
    }
    return false;
}

inline nlohmann::json bounded_validation_diagnostic(const nlohmann::json& input) {
    using J = nlohmann::json;
    if (!input.is_object() || input.value("schema_version", J()) != "shin.validation-diagnostic.v1" ||
        !input.contains("gates") || !input.at("gates").is_array()) return nullptr;
    J result{{"schema_version", "shin.validation-diagnostic.v1"}, {"gates", J::array()},
             {"truncated", input.value("truncated", J(false)) == true}};
    for (const auto& gate : input.at("gates")) {
        if (result["gates"].size() == 4) { result["truncated"] = true; break; }
        if (!gate.is_object()) continue;
        const auto id = validation_identifier(gate.value("id", J()));
        if (id.empty()) continue;
        J row{{"id", id}, {"cases", J::array()}};
        const auto error_code = validation_identifier(gate.value("error_code", J()));
        if (!error_code.empty()) row["error_code"] = error_code;
        if (gate.contains("reasons") && gate.at("reasons").is_array()) {
            for (const auto& reason : gate.at("reasons")) {
                const auto code = validation_identifier(reason);
                if (code.empty()) continue;
                if (!row.contains("reasons")) row["reasons"] = J::array();
                if (row["reasons"].size() == 3) { result["truncated"] = true; break; }
                row["reasons"].push_back(code);
            }
        }
        if (gate.contains("cases") && gate.at("cases").is_array()) for (const auto& item : gate.at("cases")) {
            if (row["cases"].size() == 2) { result["truncated"] = true; break; }
            if (!item.is_object()) continue;
            const auto case_id = validation_identifier(item.value("id", J()));
            if (case_id.empty()) continue;
            J entry{{"id", case_id}, {"reasons", J::array()}};
            if (item.contains("reasons") && item.at("reasons").is_array()) for (const auto& reason : item.at("reasons")) {
                if (entry["reasons"].size() == 3) { result["truncated"] = true; break; }
                const auto code = validation_identifier(reason);
                if (!code.empty()) entry["reasons"].push_back(code);
            }
            bool paths_truncated = result.value("truncated", false);
            append_validation_paths(entry, item.value("expected_paths", J()), paths_truncated);
            if (paths_truncated) result["truncated"] = true;
            row["cases"].push_back(std::move(entry));
        }
        result["gates"].push_back(std::move(row));
        while (result.dump().size() > 1024 && drop_last_validation_path(result))
            result["truncated"] = true;
        if (result.dump().size() > 1024) {
            result["gates"].erase(result["gates"].end() - 1);
            result["truncated"] = true;
            break;
        }
    }
    return result;
}

inline nlohmann::json bounded_failure_diagnostic(const nlohmann::json& input) {
    const auto observation = shin::observer_protocol::bounded_observation_diagnostic(input);
    return observation.is_null() ? bounded_validation_diagnostic(input) : observation;
}

inline std::string validation_failure_reason(const nlohmann::json& input) {
    const auto diagnostic = bounded_validation_diagnostic(input);
    if (diagnostic.is_null()) return {};
    for (const auto& gate : diagnostic.at("gates")) {
        for (const auto& item : gate.at("cases"))
            for (const auto& reason : item.at("reasons")) return reason.get<std::string>();
        for (const auto& reason : gate.value("reasons", nlohmann::json::array()))
            return reason.get<std::string>();
        if (gate.contains("error_code")) return gate.at("error_code").get<std::string>();
    }
    return {};
}

inline std::vector<std::string> validation_failure_expected_paths(const nlohmann::json& input) {
    const auto diagnostic = bounded_validation_diagnostic(input);
    std::vector<std::string> paths;
    if (diagnostic.is_null()) return paths;
    for (const auto& gate : diagnostic.at("gates")) {
        for (const auto& item : gate.at("cases")) {
            std::string selected_reason;
            for (const auto& reason : item.at("reasons")) {
                selected_reason = reason.get<std::string>();
                break;
            }
            if (!selected_reason.empty()) {
                if (selected_reason != "missing-required-path" && selected_reason != "missing-top-path") return {};
                const auto found = item.find("expected_paths");
                if (found == item.end() || !found->is_array()) return paths;
                for (const auto& path : *found) {
                    if (path.is_string()) paths.push_back(path.get<std::string>());
                    if (paths.size() == 2) return paths;
                }
                return paths;
            }
        }
        if (!gate.value("reasons", nlohmann::json::array()).empty() || gate.contains("error_code")) return paths;
    }
    return paths;
}

inline nlohmann::json validation_diagnostic(const nlohmann::json& validation) {
    using J = nlohmann::json;
    J result{{"schema_version", "shin.validation-diagnostic.v1"}, {"gates", J::array()}};
    for (const auto& gate : validation.value("gates", J::array())) {
        if (gate.value("status", "") != "failed") continue;
        J row{{"id", gate.at("id")}, {"cases", J::array()}};
        const auto error = gate.value("error", J());
        if (error.is_object()) row["error_code"] = error.value("code", J());
        const auto detail = gate.value("result", J());
        if (detail.is_object()) {
            row["reasons"] = J::array();
            for (const auto& issue : detail.value("issues", J::array())) {
                if (!issue.is_object() || issue.value("severity", J()) == "info") continue;
                row["reasons"].push_back(issue.value("id", issue.value("code", J())));
            }
            for (const auto& reason : detail.value("suite_failure_reasons", J::array()))
                row["reasons"].push_back(reason);
            for (const auto& reason : detail.value("failure_reasons", J::array()))
                row["reasons"].push_back(reason);
            for (const auto& failure : detail.value("failures", J::array()))
            {
                J entry{{"id", failure.value("id", J())},
                        {"reasons", failure.value("failure_reasons", J::array())}};
                J paths = J::array();
                for (const auto* key : {"missing_paths", "missing_top_paths"}) {
                    const auto found = failure.find(key);
                    if (found != failure.end() && found->is_array())
                        for (const auto& path : *found) paths.push_back(path);
                }
                if (failure.contains("primary_path")) paths.push_back(failure.at("primary_path"));
                entry["expected_paths"] = std::move(paths);
                row["cases"].push_back(std::move(entry));
            }
            for (const auto& item : detail.value("cases", J::array()))
                if (item.value("passed", true) == false)
                {
                    J entry{{"id", item.value("id", J())},
                            {"reasons", item.value("failure_reasons", J::array())}};
                    J paths = J::array();
                    for (const auto* key : {"missing_paths", "missing_top_paths"}) {
                        const auto found = item.find(key);
                        if (found != item.end() && found->is_array())
                            for (const auto& path : *found) paths.push_back(path);
                    }
                    if (item.contains("primary_path")) paths.push_back(item.at("primary_path"));
                    entry["expected_paths"] = std::move(paths);
                    row["cases"].push_back(std::move(entry));
                }
            if (detail.value("failures_truncated", 0) > 0) result["truncated"] = true;
            if (detail.value("cases_truncated", 0) > 0 ||
                detail.value("diagnostic_budget", J::object()).value("truncated", false) ||
                detail.value("failure_budget", J::object()).value("truncated", false))
                result["truncated"] = true;
        }
        result["gates"].push_back(std::move(row));
    }
    return bounded_validation_diagnostic(result);
}
// Ready wraps validate gates in steps; preserve the bounded leaf reasons.
inline nlohmann::json readiness_diagnostic(const nlohmann::json& readiness) {
    using J = nlohmann::json;
    J result{{"schema_version", "shin.validation-diagnostic.v1"}, {"gates", J::array()}};
    for (const auto& step : readiness.value("steps", J::array())) {
        if (step.value("status", "") != "failed") continue;
        auto detail = step.value("result", J());
        if (detail.is_object() && detail.contains("failed_gates")) {
            J gates = J::array();
            for (const auto& gate : detail.at("failed_gates")) {
                gates.push_back({{"id", gate.value("id", J())}, {"status", "failed"},
                    {"error", gate.value("error", J())}, {"result", gate.value("diagnostic", J())}});
            }
            detail["gates"] = std::move(gates);
        }
        if (detail.is_object() && detail.contains("gates")) {
            const auto nested = validation_diagnostic(detail);
            for (const auto& gate : nested.at("gates")) result["gates"].push_back(gate);
            if (nested.value("truncated", false)) result["truncated"] = true;
        } else {
            J gate{{"id", step.value("id", J())}, {"status", "failed"},
                   {"error", step.value("error", J())}, {"result", detail}};
            auto projected = validation_diagnostic({{"gates", J::array({gate})}});
            for (auto row : projected.at("gates")) {
                if (step.value("id", "") == "worktree-diff" && detail.is_object() && detail.value("current", true) == false)
                    row["reasons"] = J::array({"worktree-not-current"});
                result["gates"].push_back(std::move(row));
            }
        }
    }
    return bounded_validation_diagnostic(result);
}

} // namespace shin::observer
