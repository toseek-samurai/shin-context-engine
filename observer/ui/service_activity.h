#pragma once
#include "language.h"
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>
#include <array>
#include <ctime>

namespace shin::ui {
inline std::vector<std::string> mcp_activity_lines(const nlohmann::json& owner) {
    using Json = nlohmann::json;
    const auto at = owner.find("mcp_activity");
    if (at == owner.end() || !at->is_object() || at->value("schema_version", Json()) != "shin.mcp-activity.v1") return {};
    const auto& activity = *at;
    if (activity.value("status", Json()) != "ready") return {tr("MCP activity unavailable; counts are unknown.")};
    const auto count = [](const Json& value, const char* key) -> std::optional<std::uint64_t> {
        const auto it = value.find(key);
        if (it == value.end()) return std::uint64_t{0};
        if (!it->is_number_integer() || (!it->is_number_unsigned() && it->get<std::int64_t>() < 0)) return {};
        const auto number = it->get<std::uint64_t>();
        return number <= 10000 ? std::optional(number) : std::nullopt;
    };
    const auto calls = activity.value("calls", Json());
    const auto outcomes = activity.value("outcomes", Json());
    if (!calls.is_object() || !outcomes.is_object() || !activity.contains("attempts")) return {};
    const auto total = count(activity, "attempts");
    const std::array methods{"query", "inspect", "read", "status", "projects", "help", "unknown", "checkpoint", "resume"};
    std::array<std::uint64_t, 9> values{};
    std::uint64_t sum = 0;
    for (std::size_t i = 0; i < methods.size(); ++i) {
        const auto value = count(calls, methods[i]); if (!value) return {};
        values[i] = *value; sum += *value;
    }
    if (!total || sum != *total) return {};
    const auto errors = count(outcomes, "tool-error"), rpc = count(outcomes, "rpc-error"), unfinished = count(outcomes, "unfinished"), success = count(outcomes, "success");
    if (!errors || !rpc || !unfinished || !success || *errors + *rpc + *unfinished + *success != *total) return {};
    const auto n = [](std::uint64_t value) { return std::to_string(value); };
    std::vector<std::string> result{
        std::string(tr("MCP calls · history: ")) + n(*total) + tr(" · Context: ") + n(values[0] + values[1] + values[2] + values[8]),
        std::string(tr("Search: ")) + n(values[0]) + tr(" · Inspect: ") + n(values[1]) + tr(" · Read: ") + n(values[2]) + tr(" · Status: ") + n(values[3]),
        std::string(tr("Catalog: ")) + n(values[4]) + tr(" · Help: ") + n(values[5]) + tr(" · Unknown tool: ") + n(values[6]),
        std::string(tr("Tool errors: ")) + n(*errors) + tr(" · Protocol errors: ") + n(*rpc) + tr(" · Unfinished: ") + n(*unfinished),
        tr("MCP calls and service executions are separate; client receipt and task success are unknown.")};
    const auto since = activity.value("coverage_start_ms", Json());
    if (since.is_number_integer() && since.get<std::int64_t>() >= 0) {
        const auto time = static_cast<std::time_t>(since.get<std::int64_t>() / 1000);
        std::tm utc{};
#ifdef _WIN32
        const bool valid = ::gmtime_s(&utc, &time) == 0;
#else
        const bool valid = ::gmtime_r(&time, &utc) != nullptr;
#endif
        char buffer[32]{};
        if (valid && std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M", &utc))
            result.push_back(std::string(tr("Recorded since (UTC): ")) + buffer + tr(" · Up to 7 days; no backfill."));
    }
    const auto dropped = activity.value("dropped_updates", Json());
    const auto unconfirmed = activity.value("unconfirmed_updates", Json());
    if ((dropped.is_number_integer() && dropped.get<std::int64_t>() > 0) ||
        (unconfirmed.is_number_integer() && unconfirmed.get<std::int64_t>() > 0))
        result.push_back(tr("MCP history has unobserved updates; totals may be incomplete."));
    return result;
}

inline std::vector<std::string> service_activity_lines(const nlohmann::json& owner, bool compact_mcp = false) {
    auto lines = mcp_activity_lines(owner);
    if (compact_mcp && lines.size() > 1) lines.resize(1);
    const auto at = owner.find("activity");
    if (at == owner.end() || !at->is_object() || at->value("schema_version", nlohmann::json()) != "shin.service-activity.v1") return lines;
    const auto& activity = *at;
    const auto calls = activity.find("calls");
    if (calls == activity.end() || !calls->is_object()) return lines;
    const auto number = [](const nlohmann::json& value, const char* key) -> std::optional<std::string> {
        const auto it = value.find(key);
        if (it == value.end() || !it->is_number_integer() || (!it->is_number_unsigned() && it->get<std::int64_t>() < 0)) return {};
        return std::to_string(it->get<std::uint64_t>());
    };
    const auto query = number(*calls,"query"), inspect = number(*calls,"inspect"), read = number(*calls,"read"), status = number(*calls,"status");
    const auto diagnostic = number(activity,"diagnostic"), sessions = number(activity,"reported_sessions"), uncorrelated = number(activity,"uncorrelated");
    if (!query || !inspect || !read || !status || !diagnostic || !sessions || !uncorrelated) return lines;
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
    const std::vector<std::string> service{tr("Service activity · this run"),
        std::string(tr("Search: ")) + *query + tr(" · Inspect: ") + *inspect + tr(" · Read: ") + *read + tr(" · Status: ") + *status,
        std::string(tr("Diagnostic: ")) + *diagnostic + tr(" · Without session: ") + *uncorrelated,
        std::string(tr("Reported sessions: ")) + *sessions + (activity.value("sessions_truncated",nlohmann::json()) == true ? "+" : ""),
        model_line};
    lines.insert(lines.end(), service.begin(), service.end());
    return lines;
}
}
