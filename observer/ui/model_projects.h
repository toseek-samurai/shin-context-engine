#pragma once
#include "language.h"
#include "project_diagnostics.h"
#include <set>

namespace shin::ui {
// Presentation only: retain workspace IDs/roots and use an explicitly bound
// usage-group label. Multiple live checkouts remain visibly distinguishable.
inline Json project_display_labels(const Json& usage, const Json& enrolled) {
    auto result = enrolled;
    const auto groups = usage.find("projects");
    if (groups == usage.end() || !groups->is_array() || !result.is_array()) return result;
    for (const auto& group : *groups) {
        const auto keys = group.value("workspace_keys", Json::array());
        const auto matches = [&](const Json& row) {
            return std::find(keys.begin(), keys.end(), row.at("id")) != keys.end();
        };
        const auto count = std::count_if(enrolled.begin(), enrolled.end(), matches);
        for (auto& row : result) if (matches(row)) {
            const auto label = group.at("name").get<std::string>();
            row["name"] = count > 1 ? label + " / " + row.at("name").get<std::string>() : label;
        }
    }
    return result;
}
// Join only explicit workspace digests, never display names or guessed aliases.
inline Json model_project_choices(const Json& usage, const Json& enrolled) {
    Json choices = Json::array();
    std::set<std::string> mapped;
    const auto projects = usage.find("projects");
    if (projects != usage.end()) for (const auto& row : *projects) {
        const auto id = row.at("id").get<std::string>();
        choices.push_back({{"key", id.empty() ? "" : "usage/" + id}, {"name", id.empty() ? Json(tr("All usage projects")) : row.at("name")},
            {"usage_id", id}, {"configured", true}});
        for (const auto& key : row.value("workspace_keys", Json::array())) mapped.insert(key.get<std::string>());
    }
    for (const auto& row : enrolled) {
        const auto id = row.at("id").get<std::string>();
        if (mapped.contains(id)) continue;
        choices.push_back({{"key", "workspace/" + id}, {"name", row.at("name")},
            {"usage_id", nullptr}, {"configured", false}});
    }
    std::sort(choices.begin(), choices.end(), [](const auto& a, const auto& b) {
        if (a.at("key") == "" || b.at("key") == "") return a.at("key") == "" && b.at("key") != "";
        if (a.at("name") != b.at("name")) return a.at("name") < b.at("name");
        return a.at("key") < b.at("key");
    });
    return choices;
}

inline std::size_t mapped_model_projects(const Json& usage, const Json& enrolled) {
    std::size_t missing = 0;
    for (const auto& item : model_project_choices(usage, enrolled)) if (!item.at("configured").get<bool>()) ++missing;
    return enrolled.size() - missing;
}
// User-facing explanations contain only bounded counters, never client paths or IDs.
inline const char* usage_skip_reason(const std::string& code) {
    if (code == "rollout-missing") return shin::ui::tr("History file not found");
    if (code == "rollout-lines-omitted") return shin::ui::tr("Sessions with skipped long lines");
    if (code == "rollout-too-large") return shin::ui::tr("History file exceeds 4 GiB");
    if (code == "usage-byte-budget-exhausted") return shin::ui::tr("Read limit reached for this pass (4 GiB)");
    if (code == "usage-time-budget-exhausted") return shin::ui::tr("Collection deadline exceeded (120 seconds)");
    if (code == "rollout-unsafe-file") return shin::ui::tr("File failed the safe access check");
    if (code == "rollout-outside-roots") return shin::ui::tr("File outside connected history folders");
    if (code == "rollout-read-failed") return shin::ui::tr("Cannot read history file");
    if (code == "rollout-session-mismatch") return shin::ui::tr("History belongs to another session");
    if (code == "rollout-line-limit") return shin::ui::tr("History exceeds one million lines");
    if (code == "usage-counter-invalid") return shin::ui::tr("Invalid token values");
    if (code == "rollout-format-unsupported") return shin::ui::tr("Unrecognized history format");
    if (code == "usage-source-unavailable") return shin::ui::tr("Reason not specified by the previous collector");
    return shin::ui::tr("Other history read error");
}

inline std::string model_usage_coverage(const Json& usage) {
    const bool fresh = usage.value("status", "") == "available" && usage.value("fresh", false);
    std::string text = !fresh ? shin::ui::tr("Statistics stale or temporarily unavailable") :
        usage.value("complete", false) ? shin::ui::tr("Statistics updated") : shin::ui::tr("Statistics updated; some history was skipped");

    const auto sources = usage.value("sources", Json::object());
    for (const auto& source : sources.items()) {
        const std::string client = source.key() == "codex" ? "Codex" : source.key() == "hermes" ? "Hermes" : "OpenCode";
        const auto state = source.value().value("state", "");
        if (state == "available") continue;
        text += "\n" + client + ": " + (state == "partial" ? shin::ui::tr("some history unavailable") :
            state == "available" ? shin::ui::tr("history verified") : shin::ui::tr("history unavailable"));
        const auto reasons = source.value().value("skipped_reasons", Json::object());
        for (const auto& reason : reasons.items())
            text += "\n" + std::string(usage_skip_reason(reason.key())) + ": " + std::to_string(reason.value().get<std::uint64_t>());
    }
    text += shin::ui::tr("\n\nUnassigned sessions: ") + std::to_string(usage.value("unassigned_sessions", 0ULL));
    text += shin::ui::tr("\nUnread sessions: ") + std::to_string(usage.value("skipped_sessions", 0ULL));
    text += shin::ui::tr("\nTotals across all projects.");
    return text;
}

}
