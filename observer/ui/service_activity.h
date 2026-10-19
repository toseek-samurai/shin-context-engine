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
    std::string model_line = tr("Catalog calls excluded. Unclassified traffic may include checks.");
    if (const auto unknown = number(activity, "model_unknown")) {
        model_line = std::string(tr("Caller-reported models: "));
        const auto models = activity.find("models");
        if (models != activity.end() && models->is_object()) for (const auto& [name, count] : models->items()) {
            if (name.size() <= 64 && name.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_.:/-") == std::string::npos &&
                (count.is_number_unsigned() || (count.is_number_integer() && count.get<std::int64_t>() >= 0))) model_line += name + " " + std::to_string(count.get<std::uint64_t>()) + "; ";
        }
        model_line += tr("unknown: ") + *unknown;
        if (const auto overflow = number(activity, "model_overflow"); overflow && *overflow != "0") model_line += tr("; other: ") + *overflow;
    }
    return {tr("Service activity · this run"),
        std::string(tr("Search: ")) + *query + tr(" · Inspect: ") + *inspect + tr(" · Read: ") + *read + tr(" · Status: ") + *status,
        std::string(tr("Diagnostic: ")) + *diagnostic + tr(" · Without session: ") + *uncorrelated,
        std::string(tr("Reported sessions: ")) + *sessions + (activity.value("sessions_truncated",nlohmann::json()) == true ? "+" : ""),
        model_line};
}
}
