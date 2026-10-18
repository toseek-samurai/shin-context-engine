#pragma once
#include "language.h"
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

namespace shin::ui {
inline std::vector<std::string> service_activity_lines(const nlohmann::json& owner) {
    const auto at = owner.find("activity");
    if (at == owner.end() || !at->is_object() || at->value("schema_version", nlohmann::json()) != "shin.service-activity.v1") return {};
    const auto& activity = *at;
    const auto calls = activity.find("calls");
    if (calls == activity.end() || !calls->is_object()) return {};
    const auto number = [](const nlohmann::json& value, const char* key) -> std::optional<std::string> {
        const auto it = value.find(key);
        if (it == value.end() || !it->is_number_integer() || (!it->is_number_unsigned() && it->get<std::int64_t>() < 0)) return {};
        return std::to_string(it->get<std::uint64_t>());
    };
    const auto query = number(*calls,"query"), inspect = number(*calls,"inspect"), read = number(*calls,"read"), status = number(*calls,"status");
    const auto diagnostic = number(activity,"diagnostic"), sessions = number(activity,"reported_sessions"), uncorrelated = number(activity,"uncorrelated");
    if (!query || !inspect || !read || !status || !diagnostic || !sessions || !uncorrelated) return {};
    return {tr("Service activity · this run"),
        std::string(tr("Search: ")) + *query + tr(" · Inspect: ") + *inspect + tr(" · Read: ") + *read + tr(" · Status: ") + *status,
        std::string(tr("Diagnostic: ")) + *diagnostic + tr(" · Without session: ") + *uncorrelated,
        std::string(tr("Reported sessions: ")) + *sessions + (activity.value("sessions_truncated",nlohmann::json()) == true ? "+" : ""),
        tr("Catalog calls excluded. Unclassified traffic may include checks.")};
}
}
