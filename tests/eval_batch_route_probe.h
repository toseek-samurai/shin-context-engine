#pragma once
#include "context_internal.h"
#include <array>

namespace shin::context::experiment {
enum class ProbeRoute : std::size_t { ordinary, owner, directory, identifier, planner, count };
struct RouteProbeData {
    bool enabled = false;
    ProbeRoute current = ProbeRoute::ordinary;
    std::array<std::size_t, 5> scores{}, calls{};
    std::array<double, 5> wall_ms{};
};
inline thread_local RouteProbeData route_probe_data;
inline void configure_route_probe(bool enabled) { route_probe_data = {}; route_probe_data.enabled = enabled; }
inline void record_route_score() {
    if (route_probe_data.enabled) ++route_probe_data.scores[static_cast<std::size_t>(route_probe_data.current)];
}
template<class F> auto probe_route(ProbeRoute route, F&& run) {
    if (!route_probe_data.enabled) return run();
    struct Scope {
        ProbeRoute previous, route;
        SteadyClock::time_point start = SteadyClock::now();
        explicit Scope(ProbeRoute value) : previous(route_probe_data.current), route(value) {
            route_probe_data.current = route;
            ++route_probe_data.calls[static_cast<std::size_t>(route)];
        }
        ~Scope() {
            route_probe_data.wall_ms[static_cast<std::size_t>(route)] += elapsed_ms_since(start);
            route_probe_data.current = previous;
        }
    } scope(route);
    return run();
}
inline Json route_probe_metrics() {
    Json result = Json::object();
    const std::array<const char*, 5> names{"ordinary", "owner", "directory", "identifier", "planner"};
    for (std::size_t i = 0; i < names.size(); ++i)
        result[names[i]] = {{"score_calls", route_probe_data.scores[i]}, {"calls", i == 0 ? Json(nullptr) : Json(route_probe_data.calls[i])},
                           {"wall_ms", i == 0 ? Json(nullptr) : Json(route_probe_data.wall_ms[i])}};
    return result;
}
} // namespace shin::context::experiment
