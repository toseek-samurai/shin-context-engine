#pragma once

#include "source_read.h"
#include "imgui.h"
#include "shin/observer/local_service.h"
#include "shin/observer/local_installation.h"
#include "shin/observer/local_package.h"
#include "shin/observer/local_client_setup.h"
#include "shin/observer/observer.h"
#include "shin/observer/model_usage.h"
#include "../ui/project_diagnostics.h"
#include "../ui/project_progress.h"
#include "../ui/model_projects.h"
#include "../ui/request_timing.h"
#include "../ui/retained_charts.h"
#include "../ui/service_activity.h"
#include "../ui/session_activity.h"
#include "../ui/client_connections.h"
#include "profile_review.h"
#include "monitor_motion.h"
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <ctime>
#include <array>
#include <limits>
#include <utility>

namespace {
using namespace shin::observer;
using Clock = std::chrono::steady_clock;
struct ViewState {
    Json data;
    bool connected = false;
    std::string error;
    Clock::time_point received{};
    std::shared_ptr<const Json> usage = std::make_shared<Json>(Json{{"status", "unconfigured"}});
    std::shared_ptr<ProfileReviewChannel> profile_review;
    std::string agent_access_root;
    std::shared_ptr<const Json> agent_access;
};
struct ClientSettingsLocation { fs::path executable, user_home; char** environment = nullptr; };

// IPC never runs on the AppKit/render thread. No subprocess, publication or
// per-project query is triggered by viewing the panel.
class Monitor {
public:
    explicit Monitor(fs::path home, fs::path usage_file = {}, ClientSettingsLocation clients = {}) :
        home(std::move(home)), usage_path(std::move(usage_file)), clients(std::move(clients)),
        profile_review(std::make_shared<ProfileReviewChannel>()), worker([this](std::stop_token stop) { poll(stop); }) {}
    void show_agent_access(const std::string& root) {
        std::lock_guard lock(mutex);
        if (access_root == root) return;
        access_root = root;
        ++access_revision;
        state.agent_access_root.clear();
        state.agent_access.reset();
    }
    void set_usage_file(fs::path path) {
        std::lock_guard lock(mutex);
        usage_path = std::move(path); ++usage_revision;
        state.usage = std::make_shared<Json>(Json{{"status", "loading"}});
    }
    ViewState snapshot() {
        std::lock_guard lock(mutex);
        auto value = state;
        value.profile_review = profile_review;
        if (value.connected && Clock::now() - value.received > std::chrono::seconds(6)) {
            value.connected = false;
            value.error = "service-response-expired";
        }
        return value;
    }
    std::atomic<bool> visible{false};
private:
    void poll(std::stop_token stop) {
        Clock::time_point usage_due{};
        Clock::time_point access_due{};
        std::size_t loaded_revision = 0;
        while (!stop.stop_requested()) {
            try {
                LocalProjectRequest request;
                request.home = home;
                request.command = "service-status";
                const auto deadline = Clock::now() + std::chrono::seconds(2);
                auto result = request_local_service(request, [&] { return stop.stop_requested() || Clock::now() >= deadline; });
                const auto& telemetry = result.at("telemetry");
                if (telemetry.at("schema_version") != "shin.local-service-telemetry.v1" ||
                    !telemetry.at("project_states").is_array() || telemetry.at("project_states").size() > 16 ||
                    !telemetry.at("samples").is_array() || telemetry.at("samples").size() > 120 ||
                    !telemetry.at("events").is_array() || telemetry.at("events").size() > 32)
                    throw Error("telemetry-invalid", "Unexpected telemetry contract.");
                (void)result.at("query_workers").get<std::uint64_t>();
                (void)result.at("registry_error").get<std::string>();
                if (!result.at("enrollment_errors").is_object()) throw Error("telemetry-invalid", "Invalid enrollment errors.");
                (void)telemetry.at("requests_started").get<std::uint64_t>();
                (void)telemetry.at("uptime_ms").get<std::uint64_t>();
                for (const auto& row : telemetry.at("project_states")) {
                    for (const auto* key : {"id", "name", "phase", "authority", "error_code"})
                        (void)row.at(key).get<std::string>();
                    for (const auto* key : {"publications", "preparations"}) (void)row.at(key).get<std::uint64_t>();
                    (void)row.at("last_tick_ms").get<double>();
                    for (const auto* key : {"last_completed_phase", "last_error_code"})
                        if (row.contains(key)) (void)row.at(key).get<std::string>();
                    for (const auto* key : {"last_success_age_ms", "activity_age_ms"})
                        if (row.contains(key) && !row.at(key).is_null() && !shin::ui::age_ms(row, key))
                            throw Error("telemetry-invalid", "Invalid project age.");
                    if (row.contains("active_issue") && !row.at("active_issue").is_boolean())
                        throw Error("telemetry-invalid", "Invalid project issue state.");
                }
                for (const auto& row : telemetry.at("samples")) {
                    (void)row.at("elapsed_ms").get<std::uint64_t>();
                    (void)row.at("workers").get<std::uint64_t>();
                    if (!row.at("rss_bytes").is_null()) (void)row.at("rss_bytes").get<std::uint64_t>();
                }
                for (const auto& row : telemetry.at("events")) {
                    (void)row.at("name").get<std::string>();
                    (void)row.at("phase").get<std::string>();
                    (void)row.at("elapsed_ms").get<std::uint64_t>();
                }
                const auto& metrics = telemetry.at("request_metrics");
                if (!metrics.at("recent").is_array() || metrics.at("recent").size() > 64)
                    throw Error("telemetry-invalid", "Invalid request metrics.");
                for (const auto* key : {"responses_ready", "response_errors", "unobserved", "rpc_admission_rejections", "query_sample_count"})
                    (void)metrics.at(key).get<std::uint64_t>();
                for (const auto* key : {"query_p50_ms", "query_p95_ms"})
                    if (!metrics.at(key).is_null()) (void)metrics.at(key).get<double>();
                for (const auto& row : metrics.at("recent")) {
                    (void)row.at("method").get<std::string>();
                    (void)row.at("elapsed_ms").get<std::uint64_t>();
                    if (!row.at("duration_ms").is_null()) (void)row.at("duration_ms").get<double>();
                }
                const auto& history = telemetry.at("history");
                (void)history.at("status").get<std::string>();
                if (history.contains("timeline")) {
                    if (!history.at("timeline").is_array() || history.at("timeline").size() > 169 ||
                        !history.at("projects").is_array() || history.at("projects").size() > 16)
                        throw Error("telemetry-invalid", "Invalid history bounds.");
                    for (const auto& row : history.at("timeline")) {
                        if (!row.is_array() || row.size() != 10) throw Error("telemetry-invalid", "Invalid history bucket.");
                        for (const auto& value : row) if (!value.is_null()) (void)value.get<double>();
                        for (int i = 0; i < 4; ++i) (void)row.at(i).get<double>();
                    }
                    for (const auto& row : history.at("projects")) {
                        (void)row.at("id").get<std::string>();
                        for (const auto* key : {"requests", "errors", "unobserved"}) (void)row.at(key).get<std::uint64_t>();
                    }
                    if (!history.at("minute_timeline").is_array() || history.at("minute_timeline").size() > 61)
                        throw Error("telemetry-invalid", "Invalid minute history bounds.");
                    for (const auto& row : history.at("minute_timeline")) {
                        if (!row.is_array() || row.size() != 11) throw Error("telemetry-invalid", "Invalid minute bucket.");
                        for (std::size_t i = 0; i < 10; ++i) if (!row.at(i).is_null()) (void)row.at(i).get<double>();
                        if (!row.at(10).is_null()) (void)row.at(10).get<std::string>();
                    }
                }
                std::lock_guard lock(mutex);
                state.data = std::move(result); state.connected = true; state.error.clear(); state.received = Clock::now();
                const auto rows = state.data.at("telemetry").at("project_states");
                const auto review = profile_review->snapshot();
                if (review.status == "accepted-waiting") {
                    bool found = false, conflicted = false, partial = false;
                    bool success_age_available = false, has_issue = false;
                    std::optional<double> success_age;
                    std::uint64_t failures = 0;
                    std::uint64_t partial_files = 0;
                    Json partial_details = Json::array();
                    std::string phase;
                    for (const auto& row : rows) {
                        if (row.value("root", std::string{}) != review.root) continue;
                        found = true;
                        const auto code = row.value("error_code", std::string{});
                        conflicted = code == "route-revision-conflict";
                        phase = row.value("phase", std::string{});
                        failures = shin::ui::count(row, "failures");
                        has_issue = shin::ui::active_issue(row);
                        success_age_available = row.contains("last_success_age_ms");
                        const double elapsed = state.received == Clock::time_point{} ? 0 :
                            std::chrono::duration<double, std::milli>(Clock::now() - state.received).count();
                        success_age = shin::ui::age_ms(row, "last_success_age_ms", elapsed);
                        if (row.contains("source_coverage") && row.at("source_coverage").is_object()) {
                            partial = row.at("source_coverage").value("state", std::string{}) == "partial";
                            partial_files = shin::ui::count(row.at("source_coverage"), "redacted_files");
                            const auto files = row.at("source_coverage").value("files", Json::array());
                            if (files.is_array()) for (const auto& file : files) {
                                if (!file.is_object() || partial_details.size() >= 8) continue;
                                const auto path = file.value("path", std::string{});
                                const auto reason = file.value("reason", std::string{});
                                if (path.empty() || path.size() > 160 || fs::path(path).is_absolute() ||
                                    std::any_of(path.begin(), path.end(), [](unsigned char c) { return c < 32 || c == 127; })) continue;
                                partial_details.push_back({{"path", path}, {"reason", shin::context::source_redaction_rule(reason)}});
                            }
                        }
                        break;
                    }
                    profile_review->observed(review.root, success_age, success_age_available, failures,
                        !found || conflicted, has_issue, phase, partial, partial_files, std::move(partial_details));
                }
            } catch (const Error& error) {
                std::lock_guard lock(mutex);
                state.connected = false;
                state.error = error.code;
            } catch (...) {
                std::lock_guard lock(mutex);
                state.connected = false;
                state.error = "service-status-unavailable";
            }
            if (auto action = profile_review->take()) {
                const auto action_started = Clock::now();
                std::optional<double> baseline_success_age;
                bool baseline_success_age_available = false, baseline_found = false;
                std::uint64_t baseline_failures = 0;
                if (action->kind == ProfileReviewAction::Kind::accept) {
                    std::lock_guard lock(mutex);
                    const Json rows = state.data.is_object() && state.data.contains("telemetry") &&
                        state.data.at("telemetry").is_object()
                        ? state.data.at("telemetry").value("project_states", Json::array()) : Json::array();
                    const double elapsed = state.received == Clock::time_point{} ? 0 :
                        std::chrono::duration<double, std::milli>(action_started - state.received).count();
                    for (const auto& row : rows) {
                        if (row.value("root", std::string{}) != action->root) continue;
                        baseline_found = true;
                        baseline_failures = shin::ui::count(row, "failures");
                        baseline_success_age_available = row.contains("last_success_age_ms");
                        baseline_success_age = shin::ui::age_ms(row, "last_success_age_ms", elapsed);
                        break;
                    }
                }
                try {
                    auto response = request_profile_review(home, *action);
                    if (action->kind == ProfileReviewAction::Kind::preview) {
                        if (!validate_profile_review(response)) throw Error("profile-review-invalid", "Unexpected profile review contract.");
                        profile_review->preview_ready(action->root, response.at("profile_review"));
                    } else {
                        if (!response.is_object() || response.value("ok", false) != true)
                            throw Error("profile-review-invalid", "Profile approval was not accepted.");
                        if (baseline_success_age) *baseline_success_age +=
                            std::chrono::duration<double, std::milli>(Clock::now() - action_started).count();
                        if (!baseline_found) baseline_success_age_available = false;
                        profile_review->accepted(action->root, baseline_success_age, baseline_success_age_available, baseline_failures);
                    }
                } catch (const Error& error) {
                    profile_review->failed(action->root, error.code);
                } catch (...) {
                    profile_review->failed(action->root, "profile-review-unavailable");
                }
            }
            fs::path usage_file;
            std::size_t revision = 0;
            { std::lock_guard lock(mutex); usage_file = usage_path; revision = usage_revision; }
            if (revision != loaded_revision || Clock::now() >= usage_due) {
                try {
                    auto usage = read_model_usage(usage_file.empty() ? home / "usage" / "state.json" : usage_file, now_ms());
                    std::lock_guard lock(mutex);
                    if (revision == usage_revision) state.usage = std::make_shared<Json>(std::move(usage));
                } catch (...) {
                    std::lock_guard lock(mutex);
                    if (revision == usage_revision) {
                        auto prior = *state.usage; prior["status"] = "unavailable"; prior["fresh"] = false;
                        state.usage = std::make_shared<Json>(std::move(prior));
                    }
                }
                loaded_revision = revision; usage_due = Clock::now() + std::chrono::seconds(30);
            }
            std::string root;
            std::size_t requested_access = 0;
            {
                std::lock_guard lock(mutex);
                if (visible && state.connected && !access_root.empty() && !clients.executable.empty() &&
                    (state.agent_access_root != access_root || Clock::now() >= access_due)) {
                    root = access_root;
                    requested_access = access_revision;
                }
            }
            if (requested_access) {
                std::shared_ptr<const Json> access;
                try {
                    auto targets = local_client_targets(fs::canonical(clients.user_home), clients.environment);
                    auto& rows = targets.at("targets");
                    std::erase_if(rows.get_ref<Json::array_t&>(), [](const Json& row) { return !row.value("detected", false); });
                    const auto reviewed = local_client_connections(fs::canonical(clients.executable), fs::path(root), home, std::move(targets));
                    Json summary = Json::array();
                    for (const auto& row : reviewed.at("targets"))
                        summary.push_back({{"label", row.value("label", "")}, {"access_state", row.value("access_state", "review-required")}});
                    access = std::make_shared<Json>(std::move(summary));
                } catch (const std::exception&) {
                    // A client configuration that cannot be checked is not proof of a missing grant.
                }
                std::lock_guard lock(mutex);
                if (requested_access == access_revision) {
                    state.agent_access_root = root;
                    state.agent_access = std::move(access);
                    access_due = Clock::now() + std::chrono::seconds(30);
                }
            }
            const auto next = Clock::now() + (visible ? std::chrono::seconds(2) : std::chrono::seconds(5));
            while (!stop.stop_requested() && Clock::now() < next) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
    fs::path home;
    fs::path usage_path;
    ClientSettingsLocation clients;
    std::string access_root;
    std::size_t access_revision = 0;
    std::size_t usage_revision = 1;
    std::shared_ptr<ProfileReviewChannel> profile_review;
    std::mutex mutex;
    ViewState state;
    std::jthread worker;
};

void platform_action(int code);
bool platform_copy(const std::string& text);
void platform_reveal(const std::string& path);
void platform_save_view(int view);
void platform_save_language(int preference);
std::string platform_system_language();
bool platform_compact_view();
int language_preference = 0;
std::array<const char*, 3> language_choices() { return {shin::ui::tr("System"), "English", "Русский"}; }
std::string release_update_version;
void apply_language_preference(int preference) {
    language_preference = std::clamp(preference, 0, 2);
    shin::ui::set_language(shin::ui::resolve_language(language_preference == 1 ? "en" :
        language_preference == 2 ? "ru" : "auto", platform_system_language()));
}
void select_language_preference(int preference) {
    platform_save_language(preference);
    apply_language_preference(preference);
}
std::string platform_font();
std::string platform_console_font();
ImFont* console_font = nullptr;
std::string platform_version();
bool platform_reduce_motion();
void draw_brand(ImVec2 origin, float size);

// Sand frames the product. Blue identifies interaction, not project health.
constexpr ImVec4 background{.125f, .137f, .118f, 1}, raised{.169f, .180f, .149f, 1};
constexpr ImVec4 header_surface{.153f, .165f, .137f, 1};
constexpr ImVec4 hover_surface{.204f, .220f, .177f, 1}, rule{.243f, .267f, .220f, 1};
constexpr ImVec4 sand{.878f, .784f, .624f, 1}, ink{.941f, .929f, .890f, 1}, muted{.702f, .718f, .667f, 1};
constexpr ImVec4 green{.612f, .839f, .671f, 1}, amber{.906f, .761f, .490f, 1};
constexpr ImVec4 red{.957f, .659f, .616f, 1}, blue{.631f, .784f, .878f, 1};
constexpr ImVec4 idle_ink{.647f, .718f, .792f, 1};

// Native hosts may fit short pages while retaining a fixed layout budget.
// A smaller popup must not itself trigger denser typography or fewer rows.
bool panel_automatic_layout = false, panel_fit_active = false;
float panel_layout_height = 0, panel_body_budget = 0;
float layout_height() { return panel_automatic_layout ? panel_layout_height : ImGui::GetIO().DisplaySize.y; }

ImU32 alpha_color(ImVec4 color, float alpha) {
    color.w *= alpha;
    return ImGui::GetColorU32(color);
}

struct MonitorMotion {
    struct Hover { ImGuiID id = 0; shin::ui::MonitorTween tween; };
    std::array<Hover, 32> hovers{};
    shin::ui::MonitorTween nav_left, nav_width, reveal;
    std::string page;
    bool initialized = false, reduced = true, active = false;
    float delta = 0;
    void begin(const std::string& next) {
        delta = ImGui::GetIO().DeltaTime;
        reduced = platform_reduce_motion(); active = false;
        if (!initialized || delta > .5f) { reveal.snap(1); initialized = false; }
        else if (page != next) reveal.snap(.82f);
        page = next;
        reveal.step(1, delta, reduced);
        active = reveal.moving();
    }
    float hover(ImGuiID id, bool hovered) {
        auto found = std::find_if(hovers.begin(), hovers.end(), [id](const auto& item) { return item.id == id || item.id == 0; });
        auto* slot = found == hovers.end() ? &hovers[id % hovers.size()] : &*found;
        if (slot->id != id) { slot->id = id; slot->tween.snap(0); }
        const float result = slot->tween.step(hovered ? 1.0f : 0.0f, delta, reduced, .12f);
        active = active || slot->tween.moving();
        return result;
    }
} monitor_motion;

void preparation_indicator(ImVec2 origin, float size, ImVec4 color) {
    const auto center = ImVec2{origin.x + size / 2, origin.y + size / 2};
    const float turn = monitor_motion.reduced ? -1.5707963f :
        static_cast<float>(std::fmod(ImGui::GetTime(), 1.4) / 1.4) * 6.2831853f;
    auto* draw = ImGui::GetWindowDrawList();
    draw->PathArcTo(center, size * .34f, turn, turn + 4.6f, 20);
    draw->PathStroke(ImGui::GetColorU32(color), 1.4f);
}

enum class Icon { current, checking, pending, recovering, blocked, standby, stopped, offline, unknown,
                  history, requests, memory, copy, folder, back, details, more, settings, information, count };
void draw_icon(Icon icon, ImVec2 origin, float size, ImVec4 color);

Icon state_icon(shin::ui::ProjectState state) {
    using shin::ui::ProjectState;
    switch (state) {
    case ProjectState::current: return Icon::current;
    case ProjectState::checking: return Icon::checking;
    case ProjectState::pending: return Icon::pending;
    case ProjectState::recovering: return Icon::recovering;
    case ProjectState::blocked: return Icon::blocked;
    case ProjectState::standby: return Icon::standby;
    case ProjectState::stopped: return Icon::stopped;
    case ProjectState::offline: return Icon::offline;
    default: return Icon::unknown;
    }
}

ImVec4 project_color(shin::ui::ProjectState state, const Json& project) {
    if (state == shin::ui::ProjectState::blocked) return red;
    if (state == shin::ui::ProjectState::checking || state == shin::ui::ProjectState::recovering ||
        (state == shin::ui::ProjectState::pending && shin::ui::text_field(project, "query_phase") == "preparing")) return blue;
    if (state == shin::ui::ProjectState::pending) return amber;
    if (state == shin::ui::ProjectState::current) return green;
    if (state == shin::ui::ProjectState::standby) return idle_ink;
    return muted;
}

std::string clipped(std::string text, float width) {
    if (ImGui::CalcTextSize(text.c_str()).x <= width) return text;
    while (!text.empty()) {
        auto last = text.size() - 1;
        while (last > 0 && (static_cast<unsigned char>(text[last]) & 0xc0) == 0x80) --last;
        text.resize(last);
        if (ImGui::CalcTextSize((text + "…").c_str()).x <= width) return text + "…";
    }
    return "…";
}

void heading(const char* text) {
    ImGui::PushFont(nullptr, 14);
    ImGui::TextColored(muted, "%s", text);
    ImGui::PopFont();
}

void metric(const char* label, const std::string& value, ImVec4 color = ink, float size = 23) {
    if (value == "—") color = muted;
    ImGui::PushFont(nullptr, 14);
    ImGui::TextColored(muted, "%s", clipped(label, ImGui::GetContentRegionAvail().x).c_str());
    ImGui::PopFont();
    ImGui::PushFont(nullptr, size);
    const float natural_width = ImGui::CalcTextSize(value.c_str()).x;
    ImGui::PopFont();
    const float width = ImGui::GetContentRegionAvail().x;
    ImGui::PushFont(nullptr, natural_width > width ? std::max(12.0f, size * width / natural_width) : size);
    ImGui::TextColored(color, "%s", clipped(value, width).c_str());
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s: %s", label, value.c_str());
    ImGui::PopFont();
}

std::string number(double value, const char* unit) {
    char text[64];
    std::snprintf(text, sizeof(text), "%.0f %s", value, unit);
    return text;
}

std::string clock_label(std::int64_t milliseconds) {
    const auto seconds = static_cast<std::time_t>(milliseconds / 1000);
    std::tm local{};
#ifdef _WIN32
    if (::localtime_s(&local, &seconds)) return "--:--";
#else
    if (!::localtime_r(&seconds, &local)) return "--:--";
#endif
    char text[16];
    std::strftime(text, sizeof(text), "%H:%M", &local);
    return text;
}

std::string day_label(std::int64_t milliseconds) {
    const auto seconds = static_cast<std::time_t>(milliseconds / 1000);
    std::tm utc{};
#ifdef _WIN32
    if (::gmtime_s(&utc, &seconds)) return "--.--";
#else
    if (!::gmtime_r(&seconds, &utc)) return "--.--";
#endif
    char text[16];
    std::strftime(text, sizeof(text), shin::ui::language() == shin::ui::Language::ru ? "%d.%m" : "%m/%d", &utc);
    return text;
}

void graph(const Json& samples, int mode, bool connected, float height = 112) {
    const ImVec2 size{ImGui::GetContentRegionAvail().x, height};
    const float bottom = height - 12, amplitude = std::max(1.0f, bottom - 15);
    const auto origin = ImGui::GetCursorScreenPos();
    auto* draw = ImGui::GetWindowDrawList();
    const auto color = ImGui::GetColorU32(connected ? (mode == 0 ? blue : green) : muted);
    for (int i = 0; i < 3; ++i) {
        const auto y = origin.y + static_cast<float>(i) * (bottom - 4) / 2 + 4;
        draw->AddLineH(origin.x, origin.x + size.x, y, ImGui::GetColorU32(rule));
    }
    std::vector<std::pair<double, double>> points;
    for (const auto& sample : samples) {
        if (mode == 2 && sample.at("method") != "query") continue;
        const auto& value = sample.at(mode == 0 ? "rss_bytes" : mode == 1 ? "workers" : "duration_ms");
        if (!value.is_number()) continue;
        points.emplace_back(sample.at("elapsed_ms").get<double>(), value.get<double>() / (mode == 0 ? 1048576.0 : 1));
    }
    if (!points.empty()) {
        double maximum = mode == 0 ? 16 : mode == 1 ? 2 : 1;
        for (const auto& p : points) maximum = std::max(maximum, p.second * 1.15);
        const double end = points.back().first, span = std::max(60000.0, end - points.front().first);
        const double start = end - span;
        ImVec2 previous{};
        for (std::size_t i = 0; i < points.size(); ++i) {
            const ImVec2 point{origin.x + 2 + static_cast<float>((points[i].first - start) / span) * (size.x - 4),
                origin.y + bottom - static_cast<float>(points[i].second / maximum) * amplitude};
            if (mode == 2) {
                draw->AddLineV(point.x, point.y, origin.y + bottom, alpha_color(green, .7f), 2);
                draw->AddCircleFilled(point, 2.5f, color);
            } else if (i && points[i].first - points[i - 1].first < 3500) {
                draw->AddQuadFilled(previous, point, {point.x, origin.y + bottom}, {previous.x, origin.y + bottom},
                    alpha_color(blue, .06f));
                draw->AddLine(previous, point, color, 1.5f);
            }
            else draw->AddCircleFilled(point, 2, color);
            previous = point;
        }
        char label[64];
        std::snprintf(label, sizeof(label), "%.0f %s", maximum, mode == 0 ? shin::ui::tr("MiB") : mode == 2 ? shin::ui::tr("ms") : "");
        draw->AddText({origin.x + 3, origin.y + 2}, ImGui::GetColorU32(muted), label);
    }
    ImGui::Dummy(size);
    ImGui::PushFont(nullptr, 13);
    if (points.empty()) ImGui::TextColored(muted, "%s", shin::ui::tr("Waiting for the first measurement"));
    else ImGui::TextColored(muted, shin::ui::tr("%.0f s · samples: %zu"), (points.back().first - points.front().first) / 1000, points.size());
    ImGui::PopFont();
}

struct UiAction {
    std::string label, project;
    ImVec2 origin, size;
    int chart = -1;
    bool enabled = true;
    bool informational = false;
    ImGuiID item_id = 0;
    std::string target{};
};
std::vector<UiAction> ui_actions;
bool profile_accept_within_parent_clip = false;
ImVec2 profile_accept_origin{}, profile_accept_size{}, profile_accept_clip_min{}, profile_accept_clip_max{};
#include "monitor_navigation.h"

void status_badge(Icon icon, const std::string& label, ImVec4 color, int accessible_id, float size = 17) {
    const auto origin = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddCircleFilled({origin.x + size / 2, origin.y + size / 2}, size / 2,
        alpha_color(color, .12f));
    draw_icon(icon, origin, size, color);
    ImGui::Dummy({size, size});
    if (ImGui::IsItemHovered()) {
        const float wrap = std::min(360.0f, ImGui::GetIO().DisplaySize.x - 64);
        if (accessible_id == 52) {
            const float height = ImGui::CalcTextSize(label.c_str(), nullptr, false, wrap).y + 2 * ImGui::GetStyle().WindowPadding.y;
            ImGui::SetNextWindowPos({16, std::max(8.0f, std::min(origin.y + size + 8, ImGui::GetIO().DisplaySize.y - height - 8))});
        }
        ImGui::BeginTooltip();
        if (accessible_id == 52) ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrap);
        ImGui::TextUnformatted(label.c_str());
        if (accessible_id == 52) ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
    ui_actions.push_back({label, "", origin, {size, size}, accessible_id, true, true});
}
void record_action(const std::string& label, int code, bool enabled = true, const std::string& target = {}) {
    if (ImGui::IsItemVisible()) {
        const auto clip_min = ImGui::GetWindowDrawList()->GetClipRectMin(), clip_max = ImGui::GetWindowDrawList()->GetClipRectMax();
        const auto item_min = ImGui::GetItemRectMin(), item_max = ImGui::GetItemRectMax();
        const ImVec2 visible_min{std::max(item_min.x, clip_min.x), std::max(item_min.y, clip_min.y)};
        const ImVec2 visible_max{std::min(item_max.x, clip_max.x), std::min(item_max.y, clip_max.y)};
        if (visible_max.x > visible_min.x && visible_max.y > visible_min.y)
            ui_actions.push_back({label, selected, visible_min, {visible_max.x - visible_min.x, visible_max.y - visible_min.y},
                code, enabled, false, ImGui::GetItemID(), target});
    }
}

void action_button(const std::string& label, int code, float width = 0, bool enabled = true, const std::string& target = {}) {
    ImGui::PushID(code);
    ImGui::BeginDisabled(!enabled);
    const float available = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    const float natural = ImGui::CalcTextSize(label.c_str()).x + 2 * ImGui::GetStyle().FramePadding.x;
    const float requested = width > 0 ? width : natural;
    const bool pressed = ImGui::Button(label.c_str(), {std::min(available, requested), ImGui::GetFrameHeight()});
    record_action(label, code, enabled, target);
    ImGui::EndDisabled();
    ImGui::PopID();
    if (pressed) queue_panel_action(code, target);
}

void option_row(const char* label, int code, const std::string& detail = {}, bool enabled = true) {
    const auto origin = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    const float height = ImGui::GetFrameHeight() + (detail.empty() ? 4 : 17);
    const ImVec2 end{origin.x + width, origin.y + height};
    ImGui::PushID(code);
    ImGui::BeginDisabled(!enabled);
    const bool pressed = ImGui::InvisibleButton("option", {width, height}, ImGuiButtonFlags_EnableNav);
    auto* draw = ImGui::GetWindowDrawList();
    const bool hovered = ImGui::IsItemHovered();
    if (hovered) draw->AddRectFilled(origin, end, ImGui::GetColorU32(hover_surface), 4);
    if (ImGui::IsItemFocused() && ImGui::GetIO().NavVisible)
        draw->AddRect(origin, end, ImGui::GetColorU32(sand), 4);
    const auto title = clipped(label, width - 26);
    draw->AddText({origin.x + 2, origin.y + (detail.empty() ? (height - ImGui::GetFontSize()) / 2 : 4)},
        ImGui::GetColorU32(enabled ? ink : muted), title.c_str());
    draw->AddText({end.x - 11, origin.y + (height - ImGui::GetFontSize()) / 2}, ImGui::GetColorU32(muted), "›");
    if (!detail.empty()) {
        ImGui::PushFont(nullptr, 12);
        const auto shown = clipped(detail, width - 26);
        draw->AddText({origin.x + 2, origin.y + height - 16}, ImGui::GetColorU32(muted), shown.c_str());
        ImGui::PopFont();
        if (hovered && shown != detail) ImGui::SetTooltip("%s", detail.c_str());
    }
    draw->AddLineH(origin.x, end.x, end.y, ImGui::GetColorU32(rule));
    record_action(label, code, enabled);
    ImGui::EndDisabled();
    ImGui::PopID();
    if (pressed) queue_panel_action(code);
}

void primary_button(const std::string& label, int code, bool enabled = true) {
    ImGui::PushStyleColor(ImGuiCol_Button, sand);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4{.84f, .74f, .57f, 1});
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4{.77f, .67f, .51f, 1});
    ImGui::PushStyleColor(ImGuiCol_Text, background);
    action_button(label, code, 0, enabled);
    ImGui::PopStyleColor(4);
}

void begin_surface(const char* id, ImVec4 surface = raised) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, surface);
    ImGui::PushStyleColor(ImGuiCol_Border, rule);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 7);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{10, 9});
    ImGui::BeginChild(id, {0, 0}, ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY |
        ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar);
}

void end_surface() {
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
}

void page_title(const char* title) {
    ImGui::PushFont(nullptr, layout_height() < 400 ? 17.0f : 20.0f);
    ImGui::TextWrapped("%s", title);
    ImGui::PopFont();
}

void view_tab(const char* label, int code, bool active, float width, std::optional<Icon> icon = std::nullopt, bool update_notice = false) {
    const auto origin = ImGui::GetCursorScreenPos();
    const bool navigation = code == 10 || code == 11;
    const bool route = code >= 10 && code <= about;
    const float height = ImGui::GetFrameHeight();
    ImGui::PushID(code);
    if (ImGui::InvisibleButton("tab", {width, height}, ImGuiButtonFlags_EnableNav)) queue_panel_action(code);
    const bool hovered = ImGui::IsItemHovered();
    auto* draw = ImGui::GetWindowDrawList();
    const float fade = monitor_motion.hover(ImGui::GetID("tab-hover"), hovered);
    if (fade > 0) draw->AddRectFilled(origin, {origin.x + width, origin.y + height},
        alpha_color(hover_surface, fade), 4);
    if (!route) {
        draw->AddRectFilled(origin, {origin.x + width, origin.y + height}, ImGui::GetColorU32(active ? hover_surface : raised), 5);
        draw->AddRect(origin, {origin.x + width, origin.y + height}, ImGui::GetColorU32(active ? sand : rule), 5);
    }
    const float icon_width = icon ? 17.0f : 0.0f;
    const float notice_width = update_notice ? 9.0f : 0.0f;
    const auto shown = clipped(label, width - 8 - icon_width - notice_width);
    const auto text_size = ImGui::CalcTextSize(shown.c_str());
    const float left = origin.x + (width - text_size.x - icon_width - notice_width) / 2;
    const auto text_color = active ? sand : hovered || (route && !navigation) ? ink : muted;
    if (icon) draw_icon(*icon, {left, origin.y + (height - 13) / 2}, 13, text_color);
    draw->AddText({left + icon_width, origin.y + (height - text_size.y) / 2}, ImGui::GetColorU32(text_color), shown.c_str());
    if (update_notice)
        draw->AddCircleFilled({left + icon_width + text_size.x + 6, origin.y + height / 2}, 2.5f, ImGui::GetColorU32(amber));
    const auto accessibility_label = update_notice ? std::string(label) + " · " + shin::ui::tr("Update available") : label;
    if (hovered && (shown != label || update_notice)) ImGui::SetTooltip("%s", accessibility_label.c_str());
    if (navigation && active) {
        const float left = monitor_motion.nav_left.step(origin.x, monitor_motion.delta,
            monitor_motion.reduced || !monitor_motion.initialized);
        const float line_width = monitor_motion.nav_width.step(width, monitor_motion.delta,
            monitor_motion.reduced || !monitor_motion.initialized);
        monitor_motion.active = monitor_motion.active || monitor_motion.nav_left.moving() || monitor_motion.nav_width.moving();
        draw->AddLineH(left + 8, left + line_width - 8, origin.y + height - 1, ImGui::GetColorU32(sand), 2);
    }
    if (ImGui::IsItemFocused() && ImGui::GetIO().NavVisible) draw->AddRect(origin, {origin.x + width, origin.y + height}, ImGui::GetColorU32(sand), 4);
    if (ImGui::IsItemVisible()) {
        const auto clip_min = draw->GetClipRectMin(), clip_max = draw->GetClipRectMax();
        const ImVec2 lo{std::max(origin.x, clip_min.x), std::max(origin.y, clip_min.y)};
        const ImVec2 hi{std::min(origin.x + width, clip_max.x), std::min(origin.y + height, clip_max.y)};
        if (hi.x > lo.x && hi.y > lo.y) ui_actions.push_back({accessibility_label, "", lo, {hi.x - lo.x, hi.y - lo.y}, code, true, false, ImGui::GetItemID()});
    }
    ImGui::PopID();
}

#include "monitor_overview.h"

void render_request_timing(const ViewState& view, const Json& telemetry) {
    const auto summary = shin::ui::request_timing(telemetry, timing_retained, timing_filter);
    const float width = ImGui::GetContentRegionAvail().x;
    const float half = (width - 7) / 2;
    view_tab(timing_retained ? shin::ui::tr("All history ▾") : shin::ui::tr("This run ▾"), 66, false, half); ImGui::SameLine();
    view_tab(timing_filter == 0 ? shin::ui::tr("All queries ▾") : timing_filter == 1 ? shin::ui::tr("Unclassified ▾") : shin::ui::tr("Diagnostic ▾"), 62, false, half);
    const auto measurement_details = [&] {
        if (ImGui::CollapsingHeader(shin::ui::tr("Measurement details")))
            for (const auto& line : shin::ui::service_activity_lines(telemetry, true))
                ImGui::TextWrapped("%s", line.c_str());
    };
    const auto origin = ImGui::GetCursorScreenPos();
    const auto ms = [](const shin::ui::TimingMean& value) {
        const auto mean = value.value(); return mean ? number(*mean, shin::ui::tr("ms")) : std::string("—");
    };
    if (!view.connected || !summary.available || summary.requests == 0) {
        ImGui::PushFont(nullptr, 13);
        ImGui::TextColored(muted, "%s", !view.connected ? shin::ui::tr("No fresh measurements") :
            !summary.available ? shin::ui::tr("Details unavailable") : summary.invalid ? shin::ui::tr("Measurements incomplete") : shin::ui::tr("No search queries"));
        ImGui::PopFont();
        measurement_details();
        return;
    }
    if (ImGui::GetContentRegionAvail().y < 65) {
        ImGui::PushFont(nullptr, 12);
        const auto line = shin::ui::tr("Search ") + ms(summary.worker) + shin::ui::tr(" · Service ") + ms(summary.service);
        ImGui::TextColored(ink, "%s", clipped(line, width).c_str());
        ImGui::PopFont();
    } else {
        if (ImGui::BeginTable("timing-summary", 2, ImGuiTableFlags_SizingStretchSame)) {
            ImGui::TableNextColumn(); metric(shin::ui::tr("Search, mean"), ms(summary.worker), green, 22);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(shin::ui::tr("Execution in the search worker. Samples: %llu"), summary.worker.count);
            ImGui::TableNextColumn(); metric(shin::ui::tr("Including service"), ms(summary.service), blue, 22);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(shin::ui::tr("From request admission to response readiness, including preparation. Samples: %llu. Client transport is excluded."), summary.service.count);
            ImGui::EndTable();
        }
        if (ImGui::GetContentRegionAvail().y >= 23) {
            ImGui::PushFont(nullptr, 13);
            const std::string scope = timing_retained ? shin::ui::tr("Retained requests · up to 7 days") : shin::ui::tr("Last 64 requests in this run");
            ImGui::TextColored(summary.truncated || summary.invalid ? amber : muted, "%s",
                summary.truncated || summary.invalid ? shin::ui::tr("Partial detail") : scope.c_str());
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(shin::ui::tr("Search queries: %llu · Response errors: %llu\nDiagnostic: %llu · Unclassified: %llu\nUnclassified does not imply production traffic. Means include only available samples."),
                summary.requests, summary.errors, summary.diagnostic, summary.unclassified);
            ImGui::PopFont();
        }
        if (ImGui::GetContentRegionAvail().y >= 59) {
            const std::size_t rows = ImGui::GetContentRegionAvail().y >= 86 ? 3 : 2;
            const std::array labels{shin::ui::tr("Warm reader"), shin::ui::tr("With preparation"), shin::ui::tr("Unknown")};
            ImGui::PushFont(nullptr, 12);
            const double maximum = std::max({1.0, summary.readers[0].service.value().value_or(0),
                summary.readers[1].service.value().value_or(0), summary.readers[2].service.value().value_or(0)});
            for (std::size_t i = 0; i < rows; ++i) {
                const auto& group = summary.readers[i];
                const auto pos = ImGui::GetCursorScreenPos();
                auto* draw = ImGui::GetWindowDrawList();
                draw->AddText(pos, ImGui::GetColorU32(muted), labels[i]);
                const auto value = ms(group.service);
                draw->AddText({pos.x + width - ImGui::CalcTextSize(value.c_str()).x, pos.y},
                    ImGui::GetColorU32(ink), value.c_str());
                draw->AddLineH(pos.x, pos.x + width, pos.y + 18, ImGui::GetColorU32(rule), 2);
                if (const auto mean = group.service.value()) draw->AddLineH(pos.x, pos.x + width * static_cast<float>(*mean / maximum), pos.y + 18,
                    ImGui::GetColorU32(i == 0 ? green : i == 1 ? amber : muted), 2);
                ImGui::Dummy({width, 22});
                if (ImGui::IsItemHovered()) ImGui::SetTooltip(shin::ui::tr("%s · queries: %llu\nSearch: %s\nIncluding service: %s · samples: %llu"),
                    labels[i], group.requests, ms(group.worker).c_str(), value.c_str(), group.service.count);
            }
            ImGui::PopFont();
        }
        if (ImGui::GetContentRegionAvail().y >= 64) {
            const std::array labels{shin::ui::tr("Admission"), shin::ui::tr("Documents"), shin::ui::tr("Source"), shin::ui::tr("Source collection"), shin::ui::tr("Verification")};
            const double maximum = std::max({1.0, summary.phases[1].value().value_or(0), summary.phases[2].value().value_or(0), summary.phases[4].value().value_or(0)});
            ImGui::PushFont(nullptr, 13);
            for (const std::size_t i : {1U, 2U, 4U}) {
                const auto pos = ImGui::GetCursorScreenPos();
                auto* draw = ImGui::GetWindowDrawList();
                draw->AddText(pos, ImGui::GetColorU32(muted), labels[i]);
                if (const auto value = summary.phases[i].value()) draw->AddRectFilled({pos.x + 88, pos.y + 3},
                    {pos.x + 88 + (width - 160) * static_cast<float>(*value / maximum), pos.y + 11},
                    ImGui::GetColorU32(i == 2 ? blue : green), 2);
                const auto value = ms(summary.phases[i]);
                draw->AddText({pos.x + width - ImGui::CalcTextSize(value.c_str()).x, pos.y}, ImGui::GetColorU32(ink), value.c_str());
                ImGui::Dummy({width, 15});
                if (ImGui::IsItemHovered()) ImGui::SetTooltip(shin::ui::tr("%s: %s · samples: %llu\nAdmission: %s\nSource collection (within source): %s"),
                    labels[i], value.c_str(), summary.phases[i].count, ms(summary.phases[0]).c_str(), ms(summary.phases[3]).c_str());
            }
            ImGui::PopFont();
        }
    }
    std::string accessible = shin::ui::tr("Search: ") + ms(summary.worker) + shin::ui::tr("; including service: ") + ms(summary.service) +
        shin::ui::tr("; queries: ") + std::to_string(summary.requests) + shin::ui::tr("; diagnostic: ") + std::to_string(summary.diagnostic) +
        shin::ui::tr("; unclassified: ") + std::to_string(summary.unclassified) + shin::ui::tr("; warm reader: ") + ms(summary.readers[0].service) +
        shin::ui::tr("; with preparation: ") + ms(summary.readers[1].service);
    if (summary.truncated || summary.invalid) accessible += shin::ui::tr("; partial detail");
    const auto clip_min = ImGui::GetWindowDrawList()->GetClipRectMin(), clip_max = ImGui::GetWindowDrawList()->GetClipRectMax();
    const ImVec2 visible_origin{std::max(origin.x, clip_min.x), std::max(origin.y, clip_min.y)};
    const ImVec2 visible_end{std::min(origin.x + width, clip_max.x), std::min(ImGui::GetCursorScreenPos().y, clip_max.y)};
    if (visible_end.x > visible_origin.x && visible_end.y > visible_origin.y)
        ui_actions.push_back({accessible, "", visible_origin,
            {visible_end.x - visible_origin.x, visible_end.y - visible_origin.y}, 69, true, true});
    measurement_details();
}
std::string compact_number(double value) {
    char buffer[32];
    if (value >= 1000000000) std::snprintf(buffer, sizeof(buffer), "%.1fB", value / 1000000000);
    else if (value >= 1000000) std::snprintf(buffer, sizeof(buffer), "%.1fM", value / 1000000);
    else if (value >= 1000) std::snprintf(buffer, sizeof(buffer), "%.1fK", value / 1000);
    else std::snprintf(buffer, sizeof(buffer), "%.0f", value);
    return shin::ui::decimal(buffer);
}

void diagnostic_arrow(const char* id, const char* label, int code, ImGuiDir direction, bool enabled);

void usage_chart(const Json& days, bool fresh, float height) {
    const auto origin = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    auto* draw = ImGui::GetWindowDrawList();
    const float baseline = height - 8;
    for (int i = 0; i < 3; ++i) draw->AddLineH(origin.x, origin.x + width, origin.y + static_cast<float>(i) * baseline / 2, ImGui::GetColorU32(rule));
    if (!days.empty()) {
        const double day = 86400000, start = days.front().at("at_ms"), end = days.back().at("at_ms").get<double>() + day;
        double maximum = 1;
        for (const auto& row : days) maximum = std::max({maximum, row.at("input_tokens").get<double>(), row.at("output_tokens").get<double>()});
        for (const auto& row : days) {
            const float x = static_cast<float>((row.at("at_ms").get<double>() - start) / (end - start)) * width;
            const float bar = std::min(8.0f, std::max(1.0f, static_cast<float>(day / (end - start)) * width / 2 - 1));
            int index = 0;
            for (const auto* key : {"input_tokens", "output_tokens"}) {
                const float value = static_cast<float>(row.at(key).get<double>() / maximum) * (baseline - 14);
                if (value > 0) draw->AddRectFilled({origin.x + x + static_cast<float>(index) * (bar + 1), origin.y + baseline - value},
                    {origin.x + x + static_cast<float>(index) * (bar + 1) + bar, origin.y + baseline}, ImGui::GetColorU32(!fresh ? muted : index == 0 ? blue : amber));
                ++index;
            }
        }
        draw->AddText({origin.x + 2, origin.y + 1}, ImGui::GetColorU32(muted), compact_number(maximum).c_str());
    }
    ImGui::Dummy({width, height});
}

void render_models(const ViewState& view, const Json& telemetry, const Json&) {
    const auto& usage = *view.usage;
    const auto choices = shin::ui::model_project_choices(usage, telemetry.value("project_states", Json::array()));
    std::string key = model_project;
    if (!journal_project.empty()) {
        key = "workspace/" + journal_project;
        for (const auto& row : usage.value("projects", Json::array())) {
            const auto id = row.value("id", "");
            if (id.empty()) continue;
            const auto keys = row.value("workspace_keys", Json::array());
            if (std::find(keys.begin(), keys.end(), journal_project) != keys.end()) {
                key = "usage/" + id;
                break;
            }
        }
    }
    Json choice;
    for (const auto& row : choices) if (row.at("key") == key) choice = row;
    if (journal_project.empty())
        view_tab((choice.is_null() ? shin::ui::tr("Select project…") : choice.at("name").get<std::string>() + " ▾").c_str(),
            40, false, ImGui::GetContentRegionAvail().x);
    const bool fresh = usage.value("status", "") == "available" && usage.value("fresh", false);
    const bool complete = usage.value("complete", false);
    if (!usage.contains("projects")) {
        heading(shin::ui::tr("Model history"));
        ImGui::TextColored(amber, "%s", usage.value("status", "") == "unconfigured" ? shin::ui::tr("Source not connected") : shin::ui::tr("Source unavailable"));
        view_tab(shin::ui::tr("Connect history…"), 43, false, ImGui::GetContentRegionAvail().x);
        return;
    }
    if (!choice.is_null() && !choice.at("configured").get<bool>()) {
        ImGui::TextColored(amber, "%s", shin::ui::tr("Project statistics not connected"));
        ImGui::TextWrapped("%s", shin::ui::tr("The project is available to Shin. No model counters are bound to it yet."));
        return;
    }
    const Json* selected_project = nullptr;
    if (!choice.is_null()) for (const auto& row : usage.at("projects")) if (row.at("id") == choice.at("usage_id")) selected_project = &row;
    if (!selected_project) {
        ImGui::TextColored(amber, "%s", shin::ui::tr("Selected project is missing"));
        if (journal_project.empty()) view_tab(shin::ui::tr("Select project…"), 40, false, ImGui::GetContentRegionAvail().x);
        return;
    }
    const auto& project = *selected_project;
    ImGui::PushFont(nullptr, 13);
    const auto coverage = shin::ui::model_usage_coverage(usage);
    status_badge(!fresh ? Icon::pending : complete ? Icon::current : Icon::blocked,
        coverage.c_str(), fresh && complete ? muted : amber, 52, 15);
    ImGui::SameLine(); ImGui::TextColored(muted, "%s", !fresh ? shin::ui::tr("Tokens · stale data") : complete ? shin::ui::tr("Token usage") : shin::ui::tr("Tokens · incomplete history"));
    ImGui::PopFont();
    const auto& totals = project.at("totals");
    if (ImGui::BeginTable("usage-totals", 3, ImGuiTableFlags_SizingStretchSame)) {
        const char* labels[]{shin::ui::tr("Input"), shin::ui::tr("Output"), shin::ui::tr("Cache reads")};
        const char* fields[]{"input_tokens", "output_tokens", "cache_read_tokens"};
        const bool measured = !project.at("models").empty();
        for (int i = 0; i < 3; ++i) {
            ImGui::TableNextColumn(); metric(labels[i], measured ? compact_number(totals.at(fields[i])) : "—", !fresh ? muted : i == 0 ? blue : i == 1 ? amber : ink, 23);
        }
        ImGui::EndTable();
    }
    const auto& models = project.at("models");
    if (models.empty()) {
        ImGui::Spacing();
        ImGui::TextColored(muted, "%s", shin::ui::tr("No linked records for this period"));
        return;
    }
    const int pages = std::max(1, (static_cast<int>(models.size()) + 2) / 3);
    model_page = std::clamp(model_page, 0, pages - 1);
    const float reserved = 30.0f + 4 * 18.0f + (pages > 1 ? 27.0f : 0.0f);
    usage_chart(project.at("days"), fresh, std::max(64.0f, ImGui::GetContentRegionAvail().y - reserved));
    if (project.at("days").empty()) ImGui::TextColored(amber, "%s", project.at("models").empty() ? shin::ui::tr("No linked records for this project") : shin::ui::tr("Session totals available without a daily breakdown"));
    else {
        ImGui::PushFont(nullptr, 13);
        ImGui::TextColored(muted, shin::ui::tr("%s–%s UTC · input / output"), day_label(project.at("days").front().at("at_ms")).c_str(),
            day_label(project.at("days").back().at("at_ms")).c_str());
        ImGui::PopFont();
    }
    if (ImGui::BeginTable("model-records", 3, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("model", ImGuiTableColumnFlags_WidthStretch, 2.4f);
        ImGui::TableSetupColumn("input", ImGuiTableColumnFlags_WidthStretch, 1);
        ImGui::TableSetupColumn("output", ImGuiTableColumnFlags_WidthStretch, 1);
        for (const auto* title : {shin::ui::tr("Model"), shin::ui::tr("Input"), shin::ui::tr("Output")}) { ImGui::TableNextColumn(); ImGui::TextColored(muted, "%s", title); }
        for (int i = model_page * 3; i < (model_page + 1) * 3; ++i) {
            ImGui::TableNextRow(); ImGui::TableNextColumn();
            if (i >= static_cast<int>(models.size())) { ImGui::Dummy({1, ImGui::GetTextLineHeight()}); continue; }
            const auto& row = models.at(i);
            ImGui::TextUnformatted(clipped(row.at("model"), ImGui::GetContentRegionAvail().x).c_str());
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(shin::ui::tr("%s · %s\nRecords: %llu"), row.at("model").get<std::string>().c_str(), row.at("client").get<std::string>().c_str(), row.at("records").get<unsigned long long>());
            ImGui::TableNextColumn(); ImGui::TextColored(fresh ? blue : muted, "%s", compact_number(row.at("input_tokens")).c_str());
            ImGui::TableNextColumn(); ImGui::TextColored(fresh ? amber : muted, "%s", compact_number(row.at("output_tokens")).c_str());
        }
        ImGui::EndTable();
    }
    if (pages > 1) {
        diagnostic_arrow("previous-models", shin::ui::tr("Previous models"), 41, ImGuiDir_Left, model_page > 0);
        ImGui::SameLine(); ImGui::TextColored(muted, "%d / %d", model_page + 1, pages); ImGui::SameLine();
        diagnostic_arrow("next-models", shin::ui::tr("Next models"), 42, ImGuiDir_Right, model_page + 1 < pages);
    }
}

void diagnostic_arrow(const char* id, const char* label, int code, ImGuiDir direction, bool enabled) {
    const auto origin = ImGui::GetCursorScreenPos();
    ImGui::BeginDisabled(!enabled);
    if (ImGui::ArrowButton(id, direction)) queue_panel_action(code);
    ImGui::EndDisabled();
    ui_actions.push_back({label, selected, origin, ImGui::GetItemRectSize(), code, enabled});
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", label);
}

const char* profile_category_label(const std::string& id) {
    if (id == "documents") return shin::ui::tr("Project files");
    if (id == "document-selection") return shin::ui::tr("Document selection");
    if (id == "sources") return shin::ui::tr("Sources");
    if (id == "exclusions") return shin::ui::tr("Exclusions");
    if (id == "extensions") return shin::ui::tr("File types");
    if (id == "checks") return shin::ui::tr("Checks");
    if (id == "limits") return shin::ui::tr("Limits");
    if (id == "guidance") return shin::ui::tr("Instructions");
    return shin::ui::tr("Other settings");
}

void profile_review_button(const char* label, int code, bool enabled, float width = 0) {
    const auto origin = ImGui::GetCursorScreenPos();
    const ImVec2 size{width > 0 ? width : ImGui::GetContentRegionAvail().x, 30};
    ImGui::BeginDisabled(!enabled);
    const bool clicked = ImGui::Button(label, size);
    ImGui::EndDisabled();
    if (code == 71) {
        const auto* draw = ImGui::GetWindowDrawList();
        profile_accept_origin = origin;
        profile_accept_size = size;
        profile_accept_clip_min = draw->GetClipRectMin();
        profile_accept_clip_max = draw->GetClipRectMax();
        profile_accept_within_parent_clip = origin.x >= profile_accept_clip_min.x && origin.y >= profile_accept_clip_min.y &&
            origin.x + size.x <= profile_accept_clip_max.x && origin.y + size.y <= profile_accept_clip_max.y;
    }
    if (ImGui::IsRectVisible(origin, {origin.x + size.x, origin.y + size.y}))
        ui_actions.push_back({label, selected, origin, size, code, enabled});
    if (clicked && enabled) queue_panel_action(code);
}

void profile_review_message(const char* text, ImVec4 color) {
    const auto origin = ImGui::GetCursorScreenPos();
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
    const auto after = ImGui::GetCursorScreenPos();
    const ImVec2 end{origin.x + ImGui::GetContentRegionAvail().x, after.y};
    if (ImGui::IsRectVisible(origin, end))
        ui_actions.push_back({text, selected, origin, {end.x - origin.x, after.y - origin.y}, 74, true, true});
}

std::string profile_category_effect(const Json& category) {
    const auto id = category.value("id", std::string{});
    const int added = category.value("added", 0), removed = category.value("removed", 0), changed = category.value("changed", 0);
    if (id == "documents") return shin::ui::tr("The set of documents available for answers will change.");
    if (id == "document-selection") return shin::ui::tr("The paths used to select answer documents will change. Project files will stay in place.");
    if (id == "sources") return shin::ui::tr("The sources used to prepare context will change.");
    if (id == "exclusions") {
        std::string effect;
        if (added) effect += shin::ui::tr("Some paths will no longer appear in answers.");
        if (removed) effect += (effect.empty() ? "" : " ") + std::string(shin::ui::tr("Previously excluded paths will become available for answers again."));
        return effect.empty() ? shin::ui::tr("File exclusion rules will change.") : effect;
    }
    if (id == "extensions") return shin::ui::tr("The file types used for context will change.");
    if (id == "checks") return shin::ui::tr("The checks before context updates will change.");
    if (id == "limits") return shin::ui::tr("Project processing limits will change.");
    if (id == "guidance") return shin::ui::tr("Instructions affecting context preparation will change.");
    if (changed) return shin::ui::tr("An additional project processing setting will change.");
    return shin::ui::tr("The project settings will change.");
}

void render_profile_review(const ViewState& view, const std::string& current_root, const std::string& project_name) {
    const auto state = active_profile_review ? active_profile_review->snapshot() : ProfileReviewSnapshot{};
    const float width = ImGui::GetContentRegionAvail().x;
    profile_review_button(shin::ui::tr("Later"), 72, true, std::min(78.0f, width));
    ImGui::SameLine();
    ImGui::PushFont(nullptr, 15);
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(ink, "%s", clipped(shin::ui::tr("Project settings changes"), width - 90).c_str());
    ImGui::PopFont();
    ImGui::Separator();
    const float body_height = std::max(1.0f, ImGui::GetContentRegionAvail().y - 44.0f);
    ImGui::BeginChild("profile-review-body", {0, body_height}, ImGuiChildFlags_Borders, ImGuiWindowFlags_None);
    profile_review_message(project_name.c_str(), ink);
    if (state.root != current_root || state.root != profile_review_root) {
        profile_review_message(shin::ui::tr("The selected project changed. This summary no longer applies; request a new one from the project view."), amber);
        ImGui::EndChild();
        return;
    }
    if (!view.connected) {
        profile_review_message(shin::ui::tr("No service connection. Wait for reconnection and refresh the summary."), amber);
        ImGui::EndChild();
        profile_review_button(shin::ui::tr("Refresh summary"), 73, true, width);
        return;
    }
    if (state.status == "loading" || state.status == "accepting") {
        profile_review_message(state.status == "loading" ? shin::ui::tr("Checking settings… The service is still running; wait for the response.") :
            shin::ui::tr("Accepting changes… The service is still running; wait for the response."), blue);
        ImGui::EndChild();
        return;
    }
    if (state.status == "error") {
        const auto message = state.error == "service-disconnected" || state.error == "service-unavailable" ||
            state.error == "service-connect-failed" ? shin::ui::tr("The service is temporarily unavailable. Reconnect and review again.") :
            state.error == "installation-home-conflict" ? shin::ui::tr("Could not verify this installation's data directory.") :
            shin::ui::tr("The action could not be completed. Refresh the summary to check its status.");
        profile_review_message(message, amber);
        ImGui::EndChild();
        profile_review_button(shin::ui::tr("Retry"), 73, true, width);
        return;
    }
    if (state.status == "stale") {
        profile_review_message(shin::ui::tr("Settings changed after this summary was shown. Acceptance was cancelled. Request a fresh summary before deciding."), amber);
        ImGui::EndChild();
        profile_review_button(shin::ui::tr("Show current changes"), 73, true, width);
        return;
    }
    if (state.status == "accepted-waiting") {
        profile_review_message(shin::ui::tr("Changes accepted. Shin is checking the project and updating context. Project status will show readiness after the background check."), blue);
        ImGui::EndChild();
        return;
    }
    if (state.status == "success" || state.status == "partial") {
        if (state.status == "success") profile_review_message(shin::ui::tr("Changes accepted. Project context updated."), green);
        else {
            const auto message = state.partial_files
                ? std::to_string(state.partial_files) + shin::ui::tr(" files contain hidden lines. Other lines remain available.")
                : shin::ui::tr("Project context is partially available; see project status for the reason.");
            profile_review_message(message.c_str(), amber);
            for (const auto& file : state.partial_details) {
                const auto path = file.value("path", std::string{});
                const auto reason = file.value("reason", std::string{});
                const auto detail = path + " · " + std::string(shin::context::source_redaction_rule(reason));
                profile_review_message(detail.c_str(), muted);
            }
            profile_review_message(shin::ui::tr("Details are also available in project status."), muted);
        }
        ImGui::EndChild();
        return;
    }
    if (state.status == "failed") {
        profile_review_message(shin::ui::tr("Changes accepted, but the latest check failed. New context is not confirmed; open project status."), amber);
        ImGui::EndChild();
        return;
    }
    if (state.status == "unchanged") {
        profile_review_message(shin::ui::tr("No new changes found."), green);
        ImGui::EndChild();
        return;
    }
    if (!state.review.is_object()) {
        profile_review_message(shin::ui::tr("Change summary not loaded yet."), muted);
        ImGui::EndChild();
        profile_review_button(shin::ui::tr("Load summary"), 73, true, width);
        return;
    }

    const auto& review = state.review;
    profile_review_message(shin::ui::tr("Project settings have changed. Review the effect on answers before accepting."), ink);
    if (!review.value("baseline_available", false))
        profile_review_message(shin::ui::tr("The previous settings version could not be found for comparison."), amber);
    if (!review.value("checks_preserved", false))
        profile_review_message(shin::ui::tr("Preservation of prior checks is not confirmed. Consider this before accepting."), amber);
    if (review.value("checks_preserved", false))
        profile_review_message(shin::ui::tr("Prior quality checks are preserved."), muted);
    if (review.contains("valid") && !review.value("valid", false))
        profile_review_message(shin::ui::tr("New settings failed strict validation; Monitor cannot accept them."), amber);
    const auto categories = review.value("categories", Json::array());
    for (const auto& category : categories) {
        const int added = category.value("added", 0);
        const int removed = category.value("removed", 0);
        const int changed = category.value("changed", 0);
        if (!added && !removed && !changed) continue;
        const auto title = std::string(profile_category_label(category.value("id", std::string{})));
        profile_review_message(title.c_str(), ink);
        const auto counts = shin::ui::tr("Added ") + std::to_string(added) + shin::ui::tr(" · removed ") + std::to_string(removed) +
            shin::ui::tr(" · changed ") + std::to_string(changed) + ".";
        profile_review_message(counts.c_str(), muted);
        const auto effect = profile_category_effect(category);
        profile_review_message(effect.c_str(), ink);
        for (const auto* key : {"added_paths", "removed_paths"}) {
            if (!category.contains(key) || !category.at(key).is_array()) continue;
            for (const auto& path : category.at(key)) {
                if (!path.is_string()) continue;
                const auto example = std::string(key == std::string("added_paths") ? shin::ui::tr("Added path: ") : shin::ui::tr("Removed path: ")) + path.get<std::string>();
                profile_review_message(example.c_str(), muted);
            }
        }
    }
    if (ImGui::CollapsingHeader(shin::ui::tr("Technical details"))) {
        for (const auto& field : review.value("changed_fields", Json::array()))
            profile_review_message(field.get<std::string>().c_str(), muted);
        const auto revision = shin::ui::tr("Revision: ") + review.value("revision", std::string{});
        profile_review_message(revision.c_str(), muted);
    }
    const bool can_accept = review.value("can_accept", false) && (!review.contains("valid") || review.value("valid", false));
    if (!can_accept) {
        const char* reason = review.contains("valid") && !review.value("valid", false)
            ? shin::ui::tr("Correct the profile settings and validate them again first.")
            : !review.value("baseline_available", false)
                ? shin::ui::tr("Comparison requires the previous settings version. Contact the project owner.")
                : shin::ui::tr("These settings cannot be accepted in Monitor yet. The project owner must review them.");
        profile_review_message(reason, amber);
    }
    ImGui::EndChild();
    ImGui::Spacing();
    profile_review_button(shin::ui::tr("Accept changes"), 71, can_accept, width);
}

#include "monitor_project.h"

void render_projects(const ViewState& view, const Json& telemetry, const Json& history) {
    diagnostic_project.clear(); diagnostic_name.clear(); diagnostic_copy.clear(); diagnostic_root.clear();
    const auto projects = telemetry.value("project_states", Json::array());
    if (!selected.empty()) {
        for (const auto& p : projects) if (p.at("id") == selected) { render_project_diagnostics(view, p, telemetry, history); return; }
        action_button(shin::ui::tr("Back to project list"), 32);
        ImGui::TextWrapped("%s", shin::ui::tr("The project is missing from the latest service response. Its current state is unknown."));
        return;
    }
    const bool compact = layout_height() < 440;
    const bool tiny = layout_height() < 300;
    const float width = ImGui::GetContentRegionAvail().x;
    if (!tiny) page_title(shin::ui::tr("Your projects"));
    if (const auto* result = project_disconnection.result()) ImGui::TextWrapped("%s", disconnect_result_text(*result, true).c_str());
    const bool connection_problem = view.connected && shin::observer::project_connection_problem(view.data);
    if (connection_problem) {
        ImGui::PushStyleColor(ImGuiCol_Text, red);
        ImGui::TextWrapped("%s", shin::ui::tr("Some project connections could not be loaded. Check the service report."));
        ImGui::PopStyleColor();
    }
#if defined(__APPLE__) || defined(_WIN32)
    primary_button(shin::ui::tr(project_management_busy.load() ? "Operation in progress…" : "Add project…"), 80, !project_management_busy.load());
#endif
    const float row_height = tiny ? 30.0f : compact ? 44.0f : 54.0f;
    const float available = panel_fit_active ? std::max(1.0f, panel_body_budget - ImGui::GetCursorPosY()) : ImGui::GetContentRegionAvail().y;
    const auto layout = shin::ui::project_table_layout(std::max(1.0f, available - 23), static_cast<int>(projects.size()), row_height);
    project_page = std::clamp(project_page, 0, layout.pages - 1);
    char range[128];
    std::snprintf(range, sizeof(range), shin::ui::tr("Projects %d–%d of %d"), projects.empty() ? 0 : project_page * layout.page_size + 1,
        std::min((project_page + 1) * layout.page_size, static_cast<int>(projects.size())), static_cast<int>(projects.size()));
    ImGui::PushFont(nullptr, tiny ? 11.0f : 13.0f);
    ImGui::TextColored(muted, "%s", range);
    ui_actions.push_back({range, "", ImGui::GetItemRectMin(), ImGui::GetItemRectSize(), 88, true, true});
    ImGui::PopFont();
    const auto residents = view.data.is_object() ? view.data.value("resident_projects", Json::array()) : Json::array();
    const bool history_known = history.value("status", "") == "ready";
    ImGui::PushStyleColor(ImGuiCol_ChildBg, raised);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{tiny ? 4.0f : 10.0f, 4});
    const int visible_rows = std::min(layout.page_size, std::max(0, static_cast<int>(projects.size()) - project_page * layout.page_size));
    const float table_available = panel_fit_active ? panel_body_budget - ImGui::GetCursorPosY() : ImGui::GetContentRegionAvail().y;
    const float table_height = std::min(std::max(1.0f, table_available - (tiny ? 21 : 28)),
        std::max(1, visible_rows) * row_height + 8);
    ImGui::BeginChild("projects-table-region", {0, table_height}, ImGuiChildFlags_AlwaysUseWindowPadding,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    int index = 0;
    for (const auto& project : projects) {
        if (index++ / layout.page_size != project_page) continue;
        const auto id = project.at("id").get<std::string>();
        const auto name = shin::ui::text_field(project, "name");
        const auto root = shin::ui::text_field(project, "root", 4096);
        const auto presentation = shin::ui::project_presentation(project, view.connected);
        const auto state = presentation.state;
        const auto overview = shin::ui::diagnostic_overview(project, presentation);
        const bool partial = presentation.partial;
        const bool available_context = presentation.searchable();
        const bool warning = available_context && presentation.advisory();
        const auto state_label = partial || warning || !available_context ? overview.title :
            state == shin::ui::ProjectState::standby ? std::string(shin::ui::tr("Ready on demand")) : std::string(shin::ui::tr("Ready to use"));
        const auto color = available_context && (partial || warning) ? amber : project_color(state, project);
        const double elapsed = view.received == Clock::time_point{} ? 0 : std::chrono::duration<double, std::milli>(Clock::now() - view.received).count();
        const auto age = std::string(shin::ui::tr("Successful: ")) + shin::ui::age_text(shin::ui::age_ms(project, "last_success_age_ms", elapsed));
        const auto memory = shin::ui::resident_memory(residents, root, view.connected);
        const auto memory_text = memory ? number(*memory, "") : "—";
        std::uint64_t calls = 0;
        for (const auto& row : history.value("projects", Json::array())) if (row.at("id") == id) calls = shin::ui::count(row, "requests");
        const auto calls_text = history_known ? compact_number(static_cast<double>(calls)) : "—";
        const auto origin = ImGui::GetCursorScreenPos();
        const float row_width = ImGui::GetContentRegionAvail().x;
        ImGui::PushID(id.c_str());
        if (ImGui::Selectable("##open-project", false, ImGuiSelectableFlags_None, {row_width, row_height - ImGui::GetStyle().ItemSpacing.y}))
            queue_panel_action(-1, id);
        const auto item = ImGui::GetItemID();
        const bool hovered = ImGui::IsItemHovered();
        auto* draw = ImGui::GetWindowDrawList();
        if (hovered) draw->AddRectFilled(origin, {origin.x + row_width, origin.y + row_height - 4}, ImGui::GetColorU32(hover_surface), 5);
        const bool side_status = row_width >= 400;
        const float status_width = std::min(row_width * .58f, ImGui::CalcTextSize(state_label.c_str()).x + 24);
        const float name_width = side_status ? row_width - status_width - 22 : row_width - 22;
        draw->AddText(origin, ImGui::GetColorU32(hovered ? blue : ink), clipped(name, name_width).c_str());
        ImGui::PushFont(nullptr, tiny ? 11.0f : 13.0f);
        if (!tiny) {
            const ImVec2 status_at{origin.x + (side_status ? row_width - status_width - 15 : 0), origin.y + (side_status ? 0 : 22)};
            if (state == shin::ui::ProjectState::checking || state == shin::ui::ProjectState::recovering ||
                (state == shin::ui::ProjectState::pending && shin::ui::text_field(project, "query_phase") == "preparing"))
                preparation_indicator(status_at, 15, color);
            else draw_icon(available_context && (partial || warning) ? Icon::details : state_icon(state), status_at, 15, color);
            draw->AddText({status_at.x + 19, status_at.y}, ImGui::GetColorU32(color),
                clipped(state_label, side_status ? status_width - 15 : row_width - 24).c_str());
            if (side_status) draw->AddText({origin.x, origin.y + 23}, ImGui::GetColorU32(muted), clipped(age, row_width - 24).c_str());
        }
        ImGui::PopFont();
        draw->AddText({origin.x + row_width - 12, origin.y + (tiny ? 0 : 5)}, ImGui::GetColorU32(muted), "›");
        draw->AddLineH(origin.x, origin.x + row_width, origin.y + row_height - 3, alpha_color(rule, .7f));
        if (ImGui::IsItemVisible()) {
            const auto clip = draw->GetClipRectMax();
            ui_actions.push_back({name + ": " + state_label + "; " + age + "; RAM " + memory_text + shin::ui::tr(" MiB; requests: ") + calls_text,
                id, origin, {row_width, std::min(row_height - 4, clip.y - origin.y)}, -1, true, false, item});
        }
        if (hovered) ImGui::SetTooltip(shin::ui::tr("%s\n%s\n%s\n%s\nRAM: %s MiB · Retained requests: %s\nRetained check failures: %llu"),
            name.c_str(), root.c_str(), state_label.c_str(), age.c_str(), memory_text.c_str(), calls_text.c_str(),
            static_cast<unsigned long long>(shin::ui::count(project, "failures")));
        ImGui::PopID();
    }
    if (projects.empty()) ImGui::TextWrapped("%s", shin::ui::tr(view.connected ? "No connected projects" : "Project list unavailable"));
    ImGui::EndChild();
    ImGui::PopStyleVar(); ImGui::PopStyleColor();
    std::uint64_t requests = 0;
    for (const auto& row : history.value("projects", Json::array())) requests += shin::ui::count(row, "requests");
    ImGui::PushFont(nullptr, tiny ? 11.0f : 13.0f);
    ImGui::TextColored(muted, "%s", (std::string(shin::ui::tr("Requests: ")) + (history_known ? compact_number(static_cast<double>(requests)) : "—")).c_str());
    ui_actions.push_back({shin::ui::tr("All retained requests"), "", ImGui::GetItemRectMin(), ImGui::GetItemRectSize(), 53, true, true});
    if (layout.pages > 1) {
        ImGui::SameLine(std::max(ImGui::GetCursorPosX() + 4, width - 100));
        diagnostic_arrow("previous-project-page", shin::ui::tr("Previous projects"), 30, ImGuiDir_Left, project_page > 0);
        ImGui::SameLine(); ImGui::TextColored(muted, "%d/%d", project_page + 1, layout.pages); ImGui::SameLine();
        diagnostic_arrow("next-project-page", shin::ui::tr("Next projects"), 31, ImGuiDir_Right, project_page + 1 < layout.pages);
    }
    ImGui::PopFont();
}

void configure_imgui() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    monitor_motion = MonitorMotion{};
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    if (auto* font = io.Fonts->AddFontFromFileTTF(platform_font().c_str(), 14)) io.FontDefault = font;
    const auto console_path = platform_console_font();
    console_font = fs::is_regular_file(console_path) ? io.Fonts->AddFontFromFileTTF(console_path.c_str(),13) : io.FontDefault;
    ImGui::StyleColorsDark();
    auto& style = ImGui::GetStyle();
    style.WindowPadding = {16, 12};
    style.WindowRounding = 0;
    style.FrameRounding = 6;
    style.FrameBorderSize = 1;
    style.PopupRounding = 7;
    style.ChildRounding = 5;
    style.ScrollbarRounding = 4;
    style.FramePadding = {10, 5};
    style.ItemSpacing = {8, 8};
    style.Colors[ImGuiCol_WindowBg] = background;
    style.Colors[ImGuiCol_ChildBg] = background;
    style.Colors[ImGuiCol_PopupBg] = raised;
    style.Colors[ImGuiCol_Text] = ink;
    style.Colors[ImGuiCol_TextDisabled] = muted;
    style.Colors[ImGuiCol_Border] = rule;
    style.Colors[ImGuiCol_Separator] = rule;
    style.Colors[ImGuiCol_CheckMark] = sand;
    style.Colors[ImGuiCol_FrameBg] = raised;
    style.Colors[ImGuiCol_FrameBgHovered] = hover_surface;
    style.Colors[ImGuiCol_FrameBgActive] = hover_surface;
    style.Colors[ImGuiCol_Button] = raised;
    style.Colors[ImGuiCol_ButtonHovered] = hover_surface;
    style.Colors[ImGuiCol_ButtonActive] = rule;
    style.Colors[ImGuiCol_Header] = raised;
    style.Colors[ImGuiCol_HeaderHovered] = hover_surface;
    style.Colors[ImGuiCol_HeaderActive] = rule;
    style.Colors[ImGuiCol_TableHeaderBg] = background;
    style.Colors[ImGuiCol_TableRowBg] = {0, 0, 0, 0};
    style.Colors[ImGuiCol_TableRowBgAlt] = {0, 0, 0, 0};
    style.Colors[ImGuiCol_TableBorderLight] = rule;
    style.Colors[ImGuiCol_TableBorderStrong] = rule;
    style.Colors[ImGuiCol_NavCursor] = sand;
    style.Colors[ImGuiCol_SliderGrab] = sand;
    style.Colors[ImGuiCol_SliderGrabActive] = ink;
    style.Colors[ImGuiCol_ScrollbarBg] = background;
    style.Colors[ImGuiCol_ScrollbarGrab] = rule;
    style.Colors[ImGuiCol_ScrollbarGrabHovered] = muted;
    style.Colors[ImGuiCol_ScrollbarGrabActive] = sand;
    style.Colors[ImGuiCol_TextSelectedBg] = {sand.x, sand.y, sand.z, .22f};
}

#include "monitor_secondary.h"

float render(const ViewState& view, float height_limit = 0) {
    struct ResetSizing { ~ResetSizing() { panel_fit_active = panel_automatic_layout = false; } } reset_sizing;
    panel_automatic_layout = height_limit > 0;
    panel_layout_height = height_limit;
    ui_actions.clear();
    pending_panel_action.reset();
    const auto location = std::to_string(dashboard_view) + ":" + std::to_string(activity_view) + ":" + selected + ":" + model_project +
        ":" + std::to_string(static_cast<int>(project_view)) + ":" + std::to_string(static_cast<int>(settings_view)) + ":" +
        journal_project + (journal_entry ? ":entry" : "") + (timing_open ? ":timing" : "") +
        (profile_review_open ? ":review" : "") + (diagnostic_details_open ? ":checks" : "");
    monitor_motion.begin(location);
    active_profile_review = view.profile_review;
    const Json object = Json::object();
    const auto& data = view.data.is_object() ? view.data : object;
    auto telemetry = data.value("telemetry", object);
    if (telemetry.contains("project_states"))
        telemetry["project_states"] = shin::ui::project_display_labels(*view.usage, telemetry.at("project_states"));
    const auto projects = telemetry.value("project_states", Json::array());
    const auto history = telemetry.value("history", object);
    history_project.clear();
    for (const auto& row : history.value("projects", Json::array())) if (shin::ui::count(row, "errors") && history_project.empty())
        for (const auto& p : projects) if (p.at("id") == row.at("id")) history_project = p.at("id");
    if (history_project.empty()) for (const auto& p : projects) if (shin::ui::count(p, "failures")) { history_project = p.at("id"); break; }
    const auto size = ImGui::GetIO().DisplaySize;
    const bool compact = size.x < 400 || layout_height() < 440;
    const bool tiny = layout_height() < 300;
    ImGui::SetNextWindowPos({0, 0});
    ImGui::SetNextWindowSize(size);
    ImGui::SetNextWindowScroll({0, 0});
    ImGui::PushFont(nullptr, compact ? 13.0f : 14.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{compact ? 12.0f : 16.0f, tiny ? 9.0f : 12.0f});
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2{8, tiny ? 4.0f : 8.0f});
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2{compact ? 8.0f : 10.0f, tiny ? 4.0f : 5.0f});
    ImGui::PushStyleColor(ImGuiCol_WindowBg, background);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    ImGui::Begin("Shin", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleVar(); // The outer panel owns its Sand outline.
    auto* draw = ImGui::GetWindowDrawList();
    const float edge = .5f; // One outline at the panel edge, without an inset dark rim.
    draw->AddRectFilled({edge, edge}, {size.x - edge, size.y - edge}, ImGui::GetColorU32(background), 12);
    draw->AddRect({edge, edge}, {size.x - edge, size.y - edge}, alpha_color(sand, .7f), 12);
    const auto title_origin = ImGui::GetCursorScreenPos();
    const float title_height = tiny ? 28.0f : 38.0f;
    draw->AddRectFilled({edge + 1, edge + 1}, {size.x - edge - 1, title_origin.y + title_height + 8},
        ImGui::GetColorU32(header_surface), 11, ImDrawFlags_RoundTop);
    const float mark_size = tiny ? 22.0f : 26.0f;
    draw_brand({title_origin.x, title_origin.y + (title_height - mark_size) / 2}, mark_size);
    const float title_x = title_origin.x + mark_size + 10;
    const auto service_description = view.connected ? shin::ui::tr("Service connected") : shin::ui::tr("No fresh service response");
    const auto service_label = view.connected ? service_description : shin::ui::tr("No connection");
    ImGui::PushFont(nullptr, compact ? 12.0f : 14.0f);
    const auto label_width = ImGui::CalcTextSize(service_label).x;
    const float status_right = size.x - ImGui::GetStyle().WindowPadding.x;
    const ImVec2 status_origin{status_right - label_width - 17, title_origin.y + (title_height - ImGui::GetFontSize()) / 2};
    const auto color = view.connected ? green : red;
    draw->AddCircleFilled({status_origin.x + 3, status_origin.y + ImGui::GetFontSize() / 2}, 3, ImGui::GetColorU32(color));
    draw->AddText({status_origin.x + 17, status_origin.y}, ImGui::GetColorU32(color), service_label);
    ui_actions.push_back({service_description, "", status_origin, {status_right - status_origin.x, ImGui::GetFontSize()}, 50, true, true});
    ImGui::PopFont();
    ImGui::PushFont(nullptr, compact ? 15.0f : 17.0f);
    const auto title = clipped("Shin Monitor", status_origin.x - title_x - 7);
    draw->AddText({title_x, title_origin.y + (title_height - ImGui::GetFontSize()) / 2}, ImGui::GetColorU32(ink), title.c_str());
    ImGui::PopFont();
    ImGui::Dummy({0, title_height});
    if (!profile_review_open) {
        const std::array labels{shin::ui::tr("Overview"), shin::ui::tr("Projects")};
        const float available = ImGui::GetContentRegionAvail().x;
        float natural = 0;
        for (const auto* label : labels) natural += ImGui::CalcTextSize(label).x + (compact ? 12.0f : 22.0f);
        const float factor = std::min(1.0f, (available - ImGui::GetStyle().ItemSpacing.x) / natural);
        for (int i = 0; i < 2; ++i) {
            if (i) ImGui::SameLine();
            view_tab(labels[i], 10 + i, dashboard_view == i,
                (ImGui::CalcTextSize(labels[i]).x + (compact ? 12.0f : 22.0f)) * factor);
        }
    }
    const float content_top = ImGui::GetCursorPosY();
    const float footer_height = tiny ? 41.0f : 46.0f;
    const float footer_y = size.y - footer_height;
    panel_fit_active = panel_automatic_layout && !profile_review_open &&
        (dashboard_view != 2 || !timing_open);
    panel_body_budget = height_limit - content_top - footer_height - 7;
    draw->AddLineH(edge, size.x - edge, content_top - 3, ImGui::GetColorU32(rule));
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, monitor_motion.reveal.value());
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{compact ? 0.0f : 2.0f, tiny ? 2.0f : 8.0f});
    ImGui::PushStyleColor(ImGuiCol_ChildBg, background);
    static std::string prior_location;
    if (prior_location != location) {
        ImGui::SetNextWindowScroll({0, 0});
        prior_location = location;
    }
    ImGui::BeginChild("view-content", {0, std::max(1.0f, footer_y - content_top - 7)},
        ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_None);
#if defined(__APPLE__) || defined(_WIN32)
    if (projects.empty() && dashboard_view < 2 &&
        (!view.connected || (data.value("registry_error", "").empty() && data.value("enrollment_errors", Json::object()).empty()))) {
        page_title(shin::ui::tr("Connect your first project"));
        ImGui::TextWrapped("%s", shin::ui::tr("Choose a project to start using Shin. Setup also helps connect your coding agent."));
        primary_button(shin::ui::tr("Set up Shin…"), 85);
    } else
#endif
    if (dashboard_view == 0) {
        const bool restricted_height = layout_height() <= 480;
        if (restricted_height) render_readiness(view, projects);
        render_overview(view, telemetry, history);
        if (!restricted_height) render_readiness(view, projects);
    }
    else if (dashboard_view == 1) render_projects(view, telemetry, history);
    else if (dashboard_view == 2) render_activity(view, telemetry, history);
    else if (dashboard_view == 3) render_settings(view, telemetry, history);
    else render_about(view);
    const float natural_body_height = ImGui::GetCursorPosY() + ImGui::GetStyle().WindowPadding.y;
    const float preferred_height = panel_fit_active ? std::clamp(std::ceil(content_top + natural_body_height +
        footer_height + 7), std::min(280.0f, height_limit), height_limit) :
        (panel_automatic_layout ? height_limit : size.y);
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
    ImGui::SetCursorPosY(footer_y);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{0, 0});
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2{8, 0});
    ImGui::PushStyleColor(ImGuiCol_ChildBg, header_surface);
    ImGui::BeginChild("fixed-footer", {0, footer_height - 8}, ImGuiChildFlags_None,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::Separator();
    ImGui::Dummy({0, 6});
    ImGui::PushFont(nullptr, compact ? 11.0f : 13.0f);
    const auto journal = shin::ui::tr("Journal"), settings = shin::ui::tr("Settings"), about_label = shin::ui::tr("About Shin");
    const float width = ImGui::GetContentRegionAvail().x;
    const float journal_width = ImGui::CalcTextSize(journal).x + 29;
    const bool update_notice = !release_update_version.empty();
    const float settings_width = ImGui::CalcTextSize(settings).x + (update_notice ? 38 : 29);
    const float about_width = ImGui::CalcTextSize(about_label).x + 29;
    view_tab(journal, 12, dashboard_view == 2, journal_width, Icon::history);
    ImGui::SameLine(std::max(journal_width + 4, (width - settings_width) / 2));
    view_tab(settings, 13, dashboard_view == 3, settings_width, Icon::settings, update_notice);
    ImGui::SameLine(std::max(ImGui::GetCursorPosX() + 4, width - about_width));
    view_tab(about_label, about, dashboard_view == 4, about_width, Icon::information);
    ImGui::PopFont();
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(3);
    ImGui::PopFont();
    monitor_motion.initialized = true;
    apply_panel_action();
    return preferred_height;
}

} // namespace
