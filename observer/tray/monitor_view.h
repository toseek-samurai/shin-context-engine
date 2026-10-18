#pragma once
#include "imgui.h"
#include "shin/observer/local_service.h"
#include "shin/observer/local_installation.h"
#include "shin/observer/observer.h"
#include "shin/observer/model_usage.h"
#include "../ui/project_diagnostics.h"
#include "../ui/model_projects.h"
#include "../ui/request_timing.h"
#include "../ui/service_activity.h"
#include "profile_review.h"
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <ctime>
#include <array>

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
};

// IPC never runs on the AppKit/render thread. No subprocess, publication or
// per-project query is triggered by viewing the panel.
class Monitor {
public:
    explicit Monitor(fs::path home, fs::path usage_file = {}) : home(std::move(home)), usage_path(std::move(usage_file)),
        profile_review(std::make_shared<ProfileReviewChannel>()), worker([this](std::stop_token stop) { poll(stop); }) {}
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
                            partial_files = shin::ui::count(row.at("source_coverage"), "quarantined_files");
                            const auto files = row.at("source_coverage").value("files", Json::array());
                            if (files.is_array()) for (const auto& file : files) {
                                if (!file.is_object() || partial_details.size() >= 8) continue;
                                const auto path = file.value("path", std::string{});
                                const auto reason = file.value("reason", std::string{});
                                if (path.empty() || path.size() > 160 || fs::path(path).is_absolute() ||
                                    std::any_of(path.begin(), path.end(), [](unsigned char c) { return c < 32 || c == 127; })) continue;
                                partial_details.push_back({{"path", path}, {"reason", reason == "secret-like-source-content" ? reason : "unknown"}});
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
            const auto next = Clock::now() + (visible ? std::chrono::seconds(2) : std::chrono::seconds(5));
            while (!stop.stop_requested() && Clock::now() < next) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
    fs::path home;
    fs::path usage_path;
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
int language_preference = 0, applied_language_preference = 0;
Clock::time_point language_changed{};
std::string platform_font();
std::string platform_version();

constexpr ImVec4 ink{0.92f, 0.93f, 0.94f, 1}, muted{0.53f, 0.56f, 0.60f, 1};
constexpr ImVec4 green{0.35f, 0.83f, 0.64f, 1}, amber{0.97f, 0.71f, 0.36f, 1};
constexpr ImVec4 red{0.98f, 0.43f, 0.48f, 1}, blue{0.40f, 0.68f, 0.99f, 1};

enum class Icon { current, checking, pending, recovering, blocked, standby, stopped, offline, unknown,
                  history, requests, memory, copy, folder, back, details, more, count };
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

Icon phase_icon(const std::string& phase) {
    if (phase == "current") return Icon::current;
    if (phase == "checking") return Icon::checking;
    if (phase == "blocked") return Icon::blocked;
    if (phase == "coalescing") return Icon::pending;
    if (phase == "stopped" || phase == "detached") return Icon::stopped;
    return Icon::unknown;
}

const char* phase_text(const std::string& phase) {
    if (phase == "current") return shin::ui::tr("Current");
    if (phase == "checking") return shin::ui::tr("Checking files");
    if (phase == "coalescing") return shin::ui::tr("Waiting for update");
    if (phase == "blocked") return shin::ui::tr("Needs attention");
    if (phase == "stopped") return shin::ui::tr("Stopped");
    if (phase == "detached") return shin::ui::tr("Disconnected");
    return shin::ui::tr("Connecting");
}

ImVec4 phase_color(const std::string& phase, bool connected) {
    if (!connected) return muted;
    if (phase == "current") return green;
    if (phase == "blocked") return red;
    if (phase == "checking" || phase == "stopped") return muted;
    return amber;
}

ImVec4 project_color(shin::ui::ProjectState state) {
    if (state == shin::ui::ProjectState::blocked) return red;
    if (state == shin::ui::ProjectState::pending) return amber;
    if (state == shin::ui::ProjectState::recovering) return blue;
    if (state == shin::ui::ProjectState::current) return green;
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
    ImGui::Spacing();
    ImGui::PushFont(nullptr, 12);
    ImGui::TextColored(muted, "%s", text);
    ImGui::PopFont();
    ImGui::Spacing();
}

void metric(const char* label, const std::string& value, ImVec4 color = ink, float size = 27) {
    if (value == "—") color = muted;
    ImGui::PushFont(nullptr, 12);
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
    const auto color = ImGui::ColorConvertFloat4ToU32(connected ? (mode == 0 ? blue : green) : muted);
    for (int i = 0; i < 3; ++i) {
        const auto y = origin.y + static_cast<float>(i) * (bottom - 4) / 2 + 4;
        draw->AddLine({origin.x, y}, {origin.x + size.x, y}, IM_COL32(42, 47, 49, 255));
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
                draw->AddLine({point.x, origin.y + bottom}, point, IM_COL32(75, 140, 120, 180), 2);
                draw->AddCircleFilled(point, 2.5f, color);
            } else if (i && points[i].first - points[i - 1].first < 3500) {
                draw->AddQuadFilled(previous, point, {point.x, origin.y + bottom}, {previous.x, origin.y + bottom},
                    mode == 0 ? IM_COL32(85, 161, 219, 25) : IM_COL32(86, 198, 154, 25));
                draw->AddLine(previous, point, color, 1.5f);
            }
            else draw->AddCircleFilled(point, 2, color);
            previous = point;
        }
        char label[64];
        std::snprintf(label, sizeof(label), "%.0f %s", maximum, mode == 0 ? shin::ui::tr("MiB") : mode == 2 ? shin::ui::tr("ms") : "");
        draw->AddText({origin.x + 3, origin.y + 2}, IM_COL32(145, 150, 158, 255), label);
    }
    ImGui::Dummy(size);
    ImGui::PushFont(nullptr, 11);
    if (points.empty()) ImGui::TextColored(muted, "%s", shin::ui::tr("Waiting for the first measurement"));
    else ImGui::TextColored(muted, shin::ui::tr("%.0f s · samples: %zu"), (points.back().first - points.front().first) / 1000, points.size());
    ImGui::PopFont();
}

struct UiAction { std::string label, project; ImVec2 origin, size; int chart = -1; bool enabled = true; bool informational = false; };
std::vector<UiAction> ui_actions;
bool profile_accept_within_parent_clip = false;
ImVec2 profile_accept_origin{}, profile_accept_size{}, profile_accept_clip_min{}, profile_accept_clip_max{};
std::string selected;
int graph_mode = 0;
int dashboard_view = 0, history_hours = 1, project_page = 0, model_page = 0;
std::string model_project;
bool diagnostic_history_first = false;
bool timing_open = false, timing_retained = false;
int timing_filter = 0;
std::string diagnostic_project, diagnostic_copy, diagnostic_root, history_project, diagnostic_report_text;
std::atomic<bool> project_management_busy{false};
bool profile_review_open = false;
std::string profile_review_root;
std::shared_ptr<ProfileReviewChannel> active_profile_review;
void icon_button(Icon icon, const std::string& label, int code, bool enabled, ImVec2 size,
                 const std::string& value = "", bool informational = false);

void status_badge(Icon icon, const std::string& label, ImVec4 color, int accessible_id, float size = 17) {
    const auto origin = ImGui::GetCursorScreenPos();
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
Clock::time_point diagnostic_copied{};

void select_project(const std::string& id) {
    selected = selected == id ? "" : id;
    diagnostic_history_first = false;
    diagnostic_copied = {};
    profile_review_open = false;
}

void activate(int code) {
    if (code < 3) graph_mode = code;
    else if (code >= 10 && code <= 12) {
        dashboard_view = code - 10;
        platform_save_view(dashboard_view);
    } else if (code >= 20 && code <= 22) history_hours = code == 20 ? 0 : code == 21 ? 24 : 168;
    else if (code == 23) history_hours = 1;
    else if (code == 30) project_page = std::max(0, project_page - 1);
    else if (code == 31) ++project_page;
    else if (code == 32) { selected.clear(); diagnostic_history_first = false; }
    else if (code == 34) diagnostic_history_first = true;
    else if (code == 35) diagnostic_history_first = false;
    else if (code == 36 && selected == diagnostic_project && !diagnostic_copy.empty()) {
        if (platform_copy(diagnostic_copy)) diagnostic_copied = Clock::now();
    } else if (code == 37 && !history_project.empty()) {
        selected = history_project; diagnostic_history_first = true; diagnostic_copied = {};
        activate(11);
    } else if (code == 38 && selected == diagnostic_project && !diagnostic_root.empty()) {
        platform_reveal(diagnostic_root);
    }
    else if (code == 70 && active_profile_review && !profile_review_root.empty())
        profile_review_open = active_profile_review->preview(profile_review_root);
    else if (code == 71 && active_profile_review) active_profile_review->accept();
    else if (code == 72) profile_review_open = false;
    else if (code == 73 && active_profile_review && !profile_review_root.empty())
        active_profile_review->preview(profile_review_root);
    else if (code == 41) model_page = std::max(0, model_page - 1);
    else if (code == 42) ++model_page;
    else if (code == 60) { timing_open = true; timing_retained = history_hours != 0; }
    else if (code == 61) timing_open = false;
    else if (code >= 63 && code <= 65) timing_filter = code - 63;
    else if (code == 67 || code == 68) timing_retained = code == 68;
    else if (code == 80) {
        bool expected = false;
        if (project_management_busy.compare_exchange_strong(expected, true)) platform_action(code);
    } else if ((code == 39 && selected == diagnostic_project && !diagnostic_report_text.empty()) ||
        code == 40 || code == 43 || code == 44 || code == 62 || code == 66 || code == 83 || code == 84 || code == 85) platform_action(code);
}

void metric_choice(const char* label, const std::string& value, int code, ImVec4 color, float width) {
    const auto origin = ImGui::GetCursorScreenPos();
    ImGui::PushID(code);
    if (ImGui::InvisibleButton("metric", {width, 58})) activate(code);
    const bool hovered = ImGui::IsItemHovered();
    auto* draw = ImGui::GetWindowDrawList();
    ImGui::PushFont(nullptr, 11);
    draw->AddText(origin, ImGui::ColorConvertFloat4ToU32(graph_mode == code ? ink : muted), clipped(label, width).c_str());
    ImGui::PopFont();
    ImGui::PushFont(nullptr, 24);
    const auto shown = clipped(value, width);
    draw->AddText({origin.x, origin.y + 21}, ImGui::ColorConvertFloat4ToU32(value == "—" ? muted : color), shown.c_str());
    ImGui::PopFont();
    if (graph_mode == code || hovered) draw->AddLine({origin.x, origin.y + 55}, {origin.x + width, origin.y + 55},
        ImGui::ColorConvertFloat4ToU32(graph_mode == code ? color : muted), 2);
    if (hovered) ImGui::SetTooltip("%s: %s", label, value.c_str());
    ui_actions.push_back({label, "", origin, {width, 58}, code});
    ImGui::PopID();
}

void view_tab(const char* label, int code, bool active, float width) {
    const auto origin = ImGui::GetCursorScreenPos();
    ImGui::PushID(code);
    if (ImGui::InvisibleButton("tab", {width, 26})) activate(code);
    auto* draw = ImGui::GetWindowDrawList();
    if (active || ImGui::IsItemHovered()) draw->AddRectFilled(origin, {origin.x + width, origin.y + 26},
        active ? IM_COL32(50, 56, 58, 255) : IM_COL32(36, 41, 43, 255), 5);
    const auto text_size = ImGui::CalcTextSize(label);
    draw->AddText({origin.x + (width - text_size.x) / 2, origin.y + (26 - text_size.y) / 2},
        ImGui::ColorConvertFloat4ToU32(active ? ink : muted), label);
    ui_actions.push_back({label, "", origin, {width, 26}, code});
    ImGui::PopID();
}

void historical_failures_metric(const std::string& value, float size) {
    const auto origin = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    metric(shin::ui::tr("Historical errors"), value, muted, size);
    const auto after = ImGui::GetCursorScreenPos();
    ImGui::SetCursorScreenPos(origin);
    ImGui::BeginDisabled(history_project.empty());
    if (ImGui::InvisibleButton("historical-failures", {width, 44})) activate(37);
    ImGui::EndDisabled();
    ui_actions.push_back({shin::ui::tr("Historical errors: project explanation"), "", origin, {width, 44}, 37, !history_project.empty()});
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip(shin::ui::tr("Historical request failures, not current errors.%s"),
        history_project.empty() ? shin::ui::tr(" No observed project to explain.") : shin::ui::tr(" Open project explanation."));
    ImGui::SetCursorScreenPos(after);
}

void history_chart(const Json& history, int hours, bool connected, int mode = 1, float height = 112) {
    const auto origin = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    const float bottom = height - 12, amplitude = std::max(1.0f, bottom - 15);
    auto* draw = ImGui::GetWindowDrawList();
    const double now = history.value("captured_at_ms", 0.0);
    const double start = hours == 1 && history.value("oldest_at_ms", Json(nullptr)).is_number() ?
        std::min(now - 60000, std::max(now - 3600000, history.at("oldest_at_ms").get<double>())) : now - static_cast<double>(hours) * 3600000;
    const auto rows = history.value(hours == 1 ? "minute_timeline" : "timeline", Json::array());
    const double interval = hours == 1 ? 60000 : 3600000;
    const auto value_of = [mode](const Json& row) -> std::optional<double> {
        if (mode == 0) return row.at(7).is_number() ? std::optional<double>(row.at(7).get<double>() / 1048576) : std::nullopt;
        if (mode == 2) return row.at(5).get<double>() > 0 ? std::optional<double>(row.at(4).get<double>() / row.at(5).get<double>()) : std::nullopt;
        return row.at(1).get<double>();
    };
    double maximum = 1;
    std::size_t observed = 0;
    for (const auto& row : rows) if (row.at(0).get<double>() + interval > start && value_of(row)) {
        maximum = std::max(maximum, *value_of(row) * (mode == 1 ? 1 : 1.12));
        ++observed;
    }
    for (int i = 0; i < 3; ++i) draw->AddLine({origin.x, origin.y + static_cast<float>(i) * (bottom - 4) / 2 + 4},
        {origin.x + width, origin.y + static_cast<float>(i) * (bottom - 4) / 2 + 4}, IM_COL32(42, 47, 49, 255));
    ImVec2 previous{};
    double previous_at = 0;
    std::string previous_run;
    for (const auto& row : rows) {
        const auto at = row.at(0).get<double>();
        const auto measurement = value_of(row);
        if (at + interval <= start || at > now || !measurement) { previous_run.clear(); continue; }
        const auto x = std::clamp(static_cast<float>((at - start) / (now - start)), 0.0f, 1.0f) * width;
        const float bar = std::max(1.0f, width * static_cast<float>(interval / (now - start)) - 1);
        const float value = static_cast<float>(*measurement / maximum) * amplitude;
        const auto color = ImGui::ColorConvertFloat4ToU32(!connected ? muted : mode == 0 ? blue : row.at(2).get<double>() + row.at(3).get<double>() > 0 ? amber : green);
        const ImVec2 point{origin.x + x, origin.y + bottom - value};
        if (mode == 0 && hours == 1) {
            const auto run = row.at(10).is_string() ? row.at(10).get<std::string>() : "";
            if (!run.empty() && run == previous_run && at - previous_at <= interval * 1.5) {
                draw->AddQuadFilled(previous, point, {point.x, origin.y + bottom}, {previous.x, origin.y + bottom}, IM_COL32(85, 161, 219, 25));
                draw->AddLine(previous, point, color, 1.6f);
            } else draw->AddCircleFilled(point, 2, color);
            previous = point; previous_at = at; previous_run = run;
        } else draw->AddRectFilled({origin.x + x, origin.y + bottom - std::max(1.0f, value)},
            {origin.x + std::min(width, x + bar), origin.y + bottom}, color);
    }
    char scale[32];
    std::snprintf(scale, sizeof(scale), "%.0f %s", maximum, mode == 0 ? shin::ui::tr("MiB") : mode == 2 ? shin::ui::tr("ms") : hours == 1 ? shin::ui::tr("/ min") : shin::ui::tr("/ h"));
    draw->AddText({origin.x + 3, origin.y + 2}, ImGui::ColorConvertFloat4ToU32(muted), scale);
    if (!observed) draw->AddText({origin.x + 8, origin.y + bottom / 2}, ImGui::ColorConvertFloat4ToU32(muted), shin::ui::tr("No measurements for this period"));
    ImGui::Dummy({width, height});
    ImGui::PushFont(nullptr, 11);
    if (hours == 1) ImGui::TextColored(muted, shin::ui::tr("%s–%s · %zu min"), clock_label(static_cast<std::int64_t>(start)).c_str(), clock_label(static_cast<std::int64_t>(now)).c_str(), observed);
    else ImGui::TextColored(muted, shin::ui::tr("%s · hours with data: %zu"), hours == 24 ? shin::ui::tr("24 hours") : shin::ui::tr("7 days"), observed);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", shin::ui::tr("Retained observations. Boundary intervals are included in full; gaps are not zeroes. Memory and search time are interval averages."));
    ImGui::PopFont();
}

void history_state(const Json& history, bool connected, bool partial) {
    ImGui::PushFont(nullptr, 11);
    const auto state = history.value("status", "disabled");
    const bool gaps = history.value("dropped_current_run", 0ULL) || history.value("future_rows", 0ULL);
    const char* label = !connected ? shin::ui::tr("History: no fresh response") : state == "ready" ?
        (gaps ? shin::ui::tr("History: gaps present") : shin::ui::tr("History saved locally")) :
        state == "starting" ? shin::ui::tr("Connecting history") : shin::ui::tr("History unavailable");
    status_badge(!connected ? Icon::offline : state == "ready" ? (gaps ? Icon::blocked : Icon::history) :
        state == "starting" ? Icon::pending : Icon::blocked, label, connected && (gaps || state != "ready") ? amber : muted, 51, 15);
    ImGui::SameLine();
    if (connected && partial) ImGui::TextColored(muted, "%s", shin::ui::tr("Reduced detail"));
    else if (connected && state == "ready") ImGui::TextColored(muted, shin::ui::tr("History · records: %llu"), history.value("rows", 0ULL));
    else ImGui::TextColored(muted, "%s", shin::ui::tr("History"));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip(shin::ui::tr("%sRecords: %llu · %.0f KiB\n%s\nQueued: %llu · Dropped: %llu · Pruned: %llu"),
        partial ? shin::ui::tr("Some details were shortened only in this response.\nRetained history and current statuses were preserved.\n") : "",
        history.value("rows", 0ULL), history.value("database_bytes", 0.0) / 1024,
        history.value("error_code", "").c_str(), history.value("pending", 0ULL), history.value("dropped_current_run", 0ULL), history.value("pruned_current_run", 0ULL));
    ImGui::PopFont();
}

void recent_events(const Json& telemetry, const Json& history, int limit, const std::string& project = "") {
    heading(shin::ui::tr("Recent events"));
    int shown = 0;
    const auto projects = telemetry.value("project_states", Json::array());
    for (const auto& event : history.value("events", Json::array())) {
        if (!project.empty() && event.at("project_id") != project) continue;
        if (shown++ >= limit) break;
        std::string name = shin::ui::tr("Another project");
        for (const auto& p : projects) if (p.at("id") == event.at("project_id")) name = p.at("name");
        ImGui::TextColored(muted, "%s", clock_label(event.at("at_ms").get<std::int64_t>()).c_str());
        ImGui::SameLine(61);
        const auto color = phase_color(event.at("phase"), true);
        draw_icon(phase_icon(event.at("phase")), ImGui::GetCursorScreenPos(), 15, color);
        ImGui::Dummy({15, 15});
        if (ImGui::IsItemHovered()) ImGui::SetTooltip(shin::ui::tr("Historical event: %s"), phase_text(event.at("phase")));
        ImGui::SameLine();
        ImGui::TextColored(muted, "%s", clipped(name,
            ImGui::GetContentRegionAvail().x).c_str());
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s: %s", name.c_str(), phase_text(event.at("phase")));
    }
    if (!shown) ImGui::TextColored(muted, "%s", shin::ui::tr("No retained events yet"));
}

void render_overview(const ViewState& view, const Json& telemetry, const Json& history) {
    const auto metrics = telemetry.value("request_metrics", Json::object());
    std::uint64_t calls = 0, errors = 0, unknown = 0;
    double duration_sum = 0, duration_count = 0, rss_sum = 0, rss_count = 0;
    const auto since = history.value("captured_at_ms", 0.0) - static_cast<double>(history_hours) * 3600000;
    for (const auto& row : history.value(history_hours == 1 ? "minute_timeline" : "timeline", Json::array())) {
        if (row.at(0).get<double>() + (history_hours == 1 ? 60000 : 3600000) <= since) continue;
        calls += row.at(1).get<std::uint64_t>();
        errors += row.at(2).get<std::uint64_t>();
        unknown += row.at(3).get<std::uint64_t>();
        if (row.at(4).is_number()) duration_sum += row.at(4).get<double>();
        duration_count += row.at(5).get<double>();
        if (row.at(7).is_number()) { rss_sum += row.at(7).get<double>() * row.at(8).get<double>(); rss_count += row.at(8).get<double>(); }
    }
    const char* period = history_hours == 0 ? shin::ui::tr("Now ▾") : history_hours == 1 ? shin::ui::tr("1 hour ▾") : history_hours == 24 ? shin::ui::tr("24 hours ▾") : shin::ui::tr("7 days ▾");
    const float width = ImGui::GetContentRegionAvail().x;
    ImGui::TextColored(muted, "%s", history_hours ? shin::ui::tr("History") : shin::ui::tr("Run"));
    ImGui::SameLine(width - 132);
    icon_button(Icon::details, shin::ui::tr("Request timing: search, service and preparation"), 60, true, {28, 26});
    ImGui::SameLine(width - 96);
    view_tab(period, 44, false, 96);
    const auto samples = telemetry.value("samples", Json::array());
    const bool retained = history.value("status", "") == "ready";
    const bool known = view.connected && (history_hours ? retained : !metrics.empty());
    const bool memory_known = history_hours ? rss_count > 0 : !samples.empty() && samples.back().at("rss_bytes").is_number();
    const bool time_known = history_hours ? duration_count > 0 : metrics.value("query_sample_count", 0ULL) > 0;
    const double memory = history_hours ? (rss_count > 0 ? rss_sum / rss_count : 0) :
        memory_known ? samples.back().at("rss_bytes").get<double>() : 0;
    const double duration = history_hours ? (duration_count > 0 ? duration_sum / duration_count : 0) :
        time_known ? metrics.at("query_p50_ms").get<double>() : 0;
    const auto requests = history_hours ? calls : telemetry.value("requests_started", 0ULL);
    const float third = (width - 14) / 3;
    metric_choice(history_hours ? shin::ui::tr("Search, mean") : shin::ui::tr("Search, p50"), known && time_known ? number(duration, shin::ui::tr("ms")) : "—", 2, ink, third); ImGui::SameLine();
    metric_choice(history_hours ? shin::ui::tr("Memory, mean") : shin::ui::tr("Memory"), view.connected && memory_known ? number(memory / 1048576, shin::ui::tr("MiB")) : "—", 0, blue, third); ImGui::SameLine();
    metric_choice(shin::ui::tr("Requests"), known ? std::to_string(requests) : "—", 1, green, third);
    const float chart_height = std::clamp(ImGui::GetContentRegionAvail().y - 82.0f, 40.0f, 112.0f);
    if (ImGui::GetContentRegionAvail().y >= 62) {
        if (history_hours) history_chart(history, history_hours, view.connected, graph_mode, chart_height);
        else graph(graph_mode == 2 ? metrics.value("recent", Json::array()) : samples, graph_mode, view.connected, chart_height);
    }
    ImGui::Spacing();
    if (ImGui::GetContentRegionAvail().y >= 52 && ImGui::BeginTable("outcomes", 3, ImGuiTableFlags_SizingStretchSame)) {
        const std::uint64_t counts[]{history_hours ? calls - errors - unknown : metrics.value("responses_ready", 0ULL),
            history_hours ? errors : metrics.value("response_errors", 0ULL),
            history_hours ? unknown : metrics.value("unobserved", 0ULL)};
        const char* names[]{shin::ui::tr("Responses"), shin::ui::tr("Historical errors"), shin::ui::tr("Not measured")};
        for (int i = 0; i < 3; ++i) {
            ImGui::TableNextColumn();
            if (i == 1) historical_failures_metric(known ? std::to_string(counts[i]) : "—", 20);
            else metric(names[i], known ? std::to_string(counts[i]) : "—", i == 0 ? green : counts[i] ? amber : muted, 20);
        }
        ImGui::EndTable();
    }
    if (ImGui::GetContentRegionAvail().y > 54) recent_events(telemetry, history, 1);
    else if (ImGui::GetContentRegionAvail().y > 26) {
        for (const auto& event : history.value("events", Json::array())) {
            ImGui::Spacing();
            ImGui::PushFont(nullptr, 11);
            draw_icon(phase_icon(event.at("phase")), ImGui::GetCursorScreenPos(), 15, phase_color(event.at("phase"), true));
            ImGui::Dummy({15, 15});
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(shin::ui::tr("Historical event: %s"), phase_text(event.at("phase")));
            ImGui::SameLine();
            ImGui::TextColored(muted, "%s", clock_label(event.at("at_ms").get<std::int64_t>()).c_str());
            ImGui::PopFont();
            break;
        }
    }
}
void render_request_timing(const ViewState& view, const Json& telemetry) {
    const auto summary = shin::ui::request_timing(telemetry, timing_retained, timing_filter);
    const float width = ImGui::GetContentRegionAvail().x;
    icon_button(Icon::back, shin::ui::tr("Back to overview"), 61, true, {26, 26});
    ImGui::SameLine(); ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted(shin::ui::tr("Request timing"));
    const float half = (width - 7) / 2;
    view_tab(timing_retained ? shin::ui::tr("All history ▾") : shin::ui::tr("This run ▾"), 66, false, half); ImGui::SameLine();
    view_tab(timing_filter == 0 ? shin::ui::tr("All queries ▾") : timing_filter == 1 ? shin::ui::tr("Unclassified ▾") : shin::ui::tr("Diagnostic ▾"), 62, false, half);
    const auto activity = shin::ui::service_activity_lines(telemetry);
    if (!activity.empty()) {
        ImGui::PushFont(nullptr, 11);
        if (ImGui::GetContentRegionAvail().y >= 250) {
            for (const auto& line : activity) ImGui::TextWrapped("%s", line.c_str());
        } else {
            ImGui::TextWrapped("%s", activity.front().c_str());
            if (ImGui::IsItemHovered()) {
                std::string detail;
                for (const auto& line : activity) detail += line + "\n";
                ImGui::SetTooltip("%s", detail.c_str());
            }
        }
        ImGui::PopFont();
    }
    const auto origin = ImGui::GetCursorScreenPos();
    const auto ms = [](const shin::ui::TimingMean& value) {
        const auto mean = value.value(); return mean ? number(*mean, shin::ui::tr("ms")) : std::string("—");
    };
    if (!view.connected || !summary.available || summary.requests == 0) {
        ImGui::PushFont(nullptr, 13);
        ImGui::TextColored(muted, "%s", !view.connected ? shin::ui::tr("No fresh measurements") :
            !summary.available ? shin::ui::tr("Details unavailable") : summary.invalid ? shin::ui::tr("Measurements incomplete") : shin::ui::tr("No search queries"));
        ImGui::PopFont();
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
            ImGui::PushFont(nullptr, 11);
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
                draw->AddText(pos, ImGui::ColorConvertFloat4ToU32(muted), labels[i]);
                const auto value = ms(group.service);
                draw->AddText({pos.x + width - ImGui::CalcTextSize(value.c_str()).x, pos.y},
                    ImGui::ColorConvertFloat4ToU32(ink), value.c_str());
                draw->AddLine({pos.x, pos.y + 18}, {pos.x + width, pos.y + 18}, IM_COL32(42,47,49,255), 2);
                if (const auto mean = group.service.value()) draw->AddLine({pos.x, pos.y + 18},
                    {pos.x + width * static_cast<float>(*mean / maximum), pos.y + 18},
                    ImGui::ColorConvertFloat4ToU32(i == 0 ? green : i == 1 ? amber : muted), 2);
                ImGui::Dummy({width, 22});
                if (ImGui::IsItemHovered()) ImGui::SetTooltip(shin::ui::tr("%s · queries: %llu\nSearch: %s\nIncluding service: %s · samples: %llu"),
                    labels[i], group.requests, ms(group.worker).c_str(), value.c_str(), group.service.count);
            }
            ImGui::PopFont();
        }
        if (ImGui::GetContentRegionAvail().y >= 64) {
            const std::array labels{shin::ui::tr("Admission"), shin::ui::tr("Documents"), shin::ui::tr("Source"), shin::ui::tr("Source collection"), shin::ui::tr("Verification")};
            const double maximum = std::max({1.0, summary.phases[1].value().value_or(0), summary.phases[2].value().value_or(0), summary.phases[4].value().value_or(0)});
            ImGui::PushFont(nullptr, 11);
            for (const std::size_t i : {1U, 2U, 4U}) {
                const auto pos = ImGui::GetCursorScreenPos();
                auto* draw = ImGui::GetWindowDrawList();
                draw->AddText(pos, ImGui::ColorConvertFloat4ToU32(muted), labels[i]);
                if (const auto value = summary.phases[i].value()) draw->AddRectFilled({pos.x + 88, pos.y + 3},
                    {pos.x + 88 + (width - 160) * static_cast<float>(*value / maximum), pos.y + 11},
                    ImGui::ColorConvertFloat4ToU32(i == 2 ? blue : green), 2);
                const auto value = ms(summary.phases[i]);
                draw->AddText({pos.x + width - ImGui::CalcTextSize(value.c_str()).x, pos.y}, ImGui::ColorConvertFloat4ToU32(ink), value.c_str());
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
    ui_actions.push_back({accessible, "", origin,
        {width, ImGui::GetCursorScreenPos().y - origin.y}, 69, true, true});
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
    for (int i = 0; i < 3; ++i) draw->AddLine({origin.x, origin.y + static_cast<float>(i) * baseline / 2}, {origin.x + width, origin.y + static_cast<float>(i) * baseline / 2}, IM_COL32(42,47,49,255));
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
                    {origin.x + x + static_cast<float>(index) * (bar + 1) + bar, origin.y + baseline}, ImGui::ColorConvertFloat4ToU32(!fresh ? muted : index == 0 ? blue : amber));
                ++index;
            }
        }
        draw->AddText({origin.x + 2, origin.y + 1}, ImGui::ColorConvertFloat4ToU32(muted), compact_number(maximum).c_str());
    }
    ImGui::Dummy({width, height});
}

void render_models(const ViewState& view, const Json& telemetry, const Json&) {
    const auto& usage = *view.usage;
    const auto choices = shin::ui::model_project_choices(usage, telemetry.value("project_states", Json::array()));
    Json choice;
    for (const auto& row : choices) if (row.at("key") == model_project) choice = row;
    view_tab((choice.is_null() ? shin::ui::tr("Select project…") : choice.at("name").get<std::string>() + " ▾").c_str(), 40, false, ImGui::GetContentRegionAvail().x);
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
        view_tab(shin::ui::tr("Select project…"), 40, false, ImGui::GetContentRegionAvail().x);
        return;
    }
    const auto& project = *selected_project;
    ImGui::PushFont(nullptr, 11);
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
        ImGui::PushFont(nullptr, 11);
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

struct DiagnosticLine { std::string text; ImVec4 color; };

void diagnostic_lines(std::vector<DiagnosticLine>& lines, const std::string& text, float width, ImVec4 color = ink) {
    // Split at word boundaries, or UTF-8 boundaries for a long identifier.
    // The overview is bounded; the separate report retains the complete text.
    std::size_t start = 0;
    while (start < text.size()) {
        std::size_t end = start, fitting = start, space = start;
        while (end < text.size()) {
            const auto character = end;
            ++end;
            while (end < text.size() && (static_cast<unsigned char>(text[end]) & 0xc0) == 0x80) ++end;
            if (ImGui::CalcTextSize(text.substr(start, end - start).c_str()).x > width && fitting > start) break;
            fitting = end;
            if (text[character] == ' ') space = character;
        }
        if (fitting < text.size() && space > start) fitting = space;
        lines.push_back({text.substr(start, fitting - start), color});
        start = fitting;
        while (start < text.size() && text[start] == ' ') ++start;
    }
}

void diagnostic_arrow(const char* id, const char* label, int code, ImGuiDir direction, bool enabled) {
    const auto origin = ImGui::GetCursorScreenPos();
    ImGui::BeginDisabled(!enabled);
    if (ImGui::ArrowButton(id, direction)) activate(code);
    ImGui::EndDisabled();
    ui_actions.push_back({label, selected, origin, ImGui::GetItemRectSize(), code, enabled});
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", label);
}

void icon_button(Icon icon, const std::string& label, int code, bool enabled, ImVec2 size,
                 const std::string& value, bool informational) {
    const auto origin = ImGui::GetCursorScreenPos();
    ImGui::PushID(code);
    ImGui::BeginDisabled(!enabled);
    if (informational) ImGui::Dummy(size);
    else if (ImGui::InvisibleButton("icon-action", size, ImGuiButtonFlags_EnableNav)) activate(code);
    ImGui::EndDisabled();
    const bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
    if (hovered && enabled && !informational) ImGui::GetWindowDrawList()->AddRectFilled(origin,
        {origin.x + size.x, origin.y + size.y}, IM_COL32(43, 49, 52, 255), 4);
    if (!informational && ImGui::IsItemFocused()) ImGui::GetWindowDrawList()->AddRect(origin,
        {origin.x + size.x, origin.y + size.y}, ImGui::ColorConvertFloat4ToU32(blue), 4);
    draw_icon(icon, {origin.x + (value.empty() ? (size.x - 17) / 2 : 4), origin.y + shin::ui::centered_item_y(size.y, 17)},
        17, enabled ? ink : muted);
    if (!value.empty()) ImGui::GetWindowDrawList()->AddText({origin.x + 25, origin.y + shin::ui::centered_item_y(size.y, ImGui::GetFontSize())},
        ImGui::ColorConvertFloat4ToU32(muted), clipped(value, size.x - 25).c_str());
    if (hovered) ImGui::SetTooltip("%s", label.c_str());
    ui_actions.push_back({label, selected, origin, size, code, enabled, informational});
    ImGui::PopID();
}

void diagnostic_text(const std::string& text, float width, float size, ImVec4 color, int max_lines, int accessible_id) {
    const auto origin = ImGui::GetCursorScreenPos();
    ImGui::PushFont(nullptr, size);
    std::vector<DiagnosticLine> lines;
    diagnostic_lines(lines, text, width, color);
    const int shown = std::min(max_lines, static_cast<int>(lines.size()));
    for (int i = 0; i < shown; ++i) {
        const auto value = i == shown - 1 && shown < static_cast<int>(lines.size()) ?
            clipped(lines[i].text + "…", width) : lines[i].text;
        ImGui::TextColored(color, "%s", value.c_str());
    }
    if (shown < static_cast<int>(lines.size()) && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", text.c_str());
    ImGui::PopFont();
    ui_actions.push_back({text, selected, origin, {width, ImGui::GetCursorScreenPos().y - origin.y},
        accessible_id, true, true});
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
    if (clicked && enabled) activate(code);
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
                ? std::to_string(state.partial_files) + shin::ui::tr(" files unavailable. Context was updated from the remaining data.")
                : shin::ui::tr("Project context is partially available; see project status for the reason.");
            profile_review_message(message.c_str(), amber);
            for (const auto& file : state.partial_details) {
                const auto path = file.value("path", std::string{});
                const auto reason = file.value("reason", std::string{});
                const auto detail = reason == "secret-like-source-content"
                    ? shin::ui::tr("Excluded file ") + path + shin::ui::tr(": the check found a possible secret.")
                    : shin::ui::tr("Unavailable file: ") + path + ".";
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

void render_project_diagnostics(const ViewState& view, const Json& project, const Json& telemetry, const Json& history) {
    using namespace shin::ui;
    diagnostic_project = text_field(project, "id", 96);
    const auto root = project.contains("root") && project.at("root").is_string() ? project.at("root").get<std::string>() : std::string{};
    active_profile_review = view.profile_review;
    if (!profile_review_open) profile_review_root = root;
    if (root.size() <= 4096 && !root.empty() && fs::path(root).is_absolute() &&
        std::none_of(root.begin(), root.end(), [](unsigned char c) { return c < 32 || c == 127; })) diagnostic_root = root;
    diagnostic_copy = diagnostic_summary(project, view.data, view.connected, view.error,
        platform_version());
    const auto state = project_state(project, view.connected);
    const auto overview = diagnostic_overview(project, view.connected);
    const auto name = text_field(project, "name");
    const auto prior_code = historical_code(project);
    auto current_code = view.connected ? error_code(project, "error_code") : error_code(Json{{"code", view.error}}, "code");
    if (current_code.empty() && active_issue(project)) current_code = prior_code;
    const auto explanation = current_code.empty() || (view.connected && active_issue(project) &&
        (current_code == "refresh-validation-failed" || current_code == "project-check-failed"))
        ? Explanation{overview.summary, overview.next} : explain(current_code);
    if (profile_review_open) {
        render_profile_review(view, root, name);
        return;
    }
    diagnostic_report_text = name + "\n\n" + overview.title + "\n" + explanation.cause +
        shin::ui::tr("\n\nNext step\n") + explanation.next + shin::ui::tr("\n\nHistory\n") +
        diagnostic_history_status(project, view.connected) +
        (prior_code.empty() ? "" : shin::ui::tr("\nLast code: ") + prior_code + "\n" + explain(prior_code).cause) +
        shin::ui::tr("\n\nTechnical details of the last observation\n") + diagnostic_copy;
    const auto* retained = retained_project(project, history);
    const double elapsed = view.received == Clock::time_point{} ? 0 :
        std::chrono::duration<double, std::milli>(Clock::now() - view.received).count();
    const auto age = age_text(age_ms(project, "last_success_age_ms", elapsed));
    const float width = ImGui::GetContentRegionAvail().x;
    const float top = ImGui::GetCursorPosY(), height = ImGui::GetContentRegionAvail().y;
    const float footer = top + height - 32;
    const bool compact_diagnostics = height < 230, minimal = height < 165;

    icon_button(Icon::back, diagnostic_history_first ? shin::ui::tr("Back to project status") : shin::ui::tr("Back to project list"),
        diagnostic_history_first ? 35 : 32, true, {28, 28});
    ImGui::SameLine();
    ImGui::PushFont(nullptr, 14);
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(ink, "%s", clipped(name, width - 76).c_str());
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", name.c_str());
    ImGui::PopFont();
    ImGui::SameLine(width - 28);
    icon_button(Icon::details, shin::ui::tr("Open full report"), 39, true, {28, 28});
    ImGui::SetCursorPosY(top + 38);

    if (!diagnostic_history_first) {
        const auto origin = ImGui::GetCursorScreenPos();
        draw_icon(state_icon(state), {origin.x, origin.y + 1}, 24, project_color(state));
        ImGui::SetCursorPosX(32);
        diagnostic_text(overview.title, width - 32, minimal ? 14.0f : compact_diagnostics ? 16.0f : 18.0f, project_color(state), minimal ? 1 : 2, 54);
        ImGui::SetCursorPosX(0);
        if (!minimal) {
            ImGui::Spacing();
            diagnostic_text(overview.summary, width, 14, ink, compact_diagnostics ? 2 : 3, 55);
            ImGui::Spacing();
            diagnostic_text(shin::ui::tr("Next step"), width, 12, muted, 1, 56);
            diagnostic_text(overview.next, width, 14, ink, 2, 57);
            if (footer - ImGui::GetCursorPosY() >= 48) {
                ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
                const auto success_origin = ImGui::GetCursorScreenPos();
                ImGui::PushFont(nullptr, 12);
                ImGui::TextColored(muted, "%s", shin::ui::tr("Successful check"));
                ImGui::SameLine(width - ImGui::CalcTextSize(age.c_str()).x);
                ImGui::TextColored(ink, "%s", age.c_str());
                ImGui::PopFont();
                ui_actions.push_back({shin::ui::tr("Successful check: ") + age, selected, success_origin, {width, 16}, 58, true, true});
            }
            const auto counts_origin = ImGui::GetCursorScreenPos();
            if (footer - ImGui::GetCursorPosY() >= 76 && ImGui::BeginTable("diagnostic-overview-counts", 2, ImGuiTableFlags_SizingStretchSame)) {
                ImGui::TableNextColumn();
                metric(shin::ui::tr("Updates"), project.contains("publications") ? std::to_string(count(project, "publications")) : "—", ink, 23);
                ImGui::PushFont(nullptr, 11); ImGui::TextColored(muted, "%s", shin::ui::tr("This service run")); ImGui::PopFont();
                ImGui::TableNextColumn();
                metric(shin::ui::tr("Requests"), retained ? compact_number(static_cast<double>(count(*retained, "requests"))) : "—", ink, 23);
                if (ImGui::IsItemHovered()) {
                    std::string detail;
                    for (const auto& line : shin::ui::service_activity_lines(project)) detail += line + "\n";
                    if (!detail.empty()) ImGui::SetTooltip("%s", detail.c_str());
                }
                ImGui::PushFont(nullptr, 11); ImGui::TextColored(muted, "%s", shin::ui::tr("Retained history")); ImGui::PopFont();
                ImGui::EndTable();
                ui_actions.push_back({shin::ui::tr("Updates in this service run: ") + (project.contains("publications") ? std::to_string(count(project, "publications")) : shin::ui::tr("unknown")) +
                    shin::ui::tr(". Retained requests: ") + (retained ? std::to_string(count(*retained, "requests")) : shin::ui::tr("unknown")), selected,
                    counts_origin, {width, ImGui::GetCursorScreenPos().y - counts_origin.y}, 59, true, true});
            }
        }
    } else {
        diagnostic_text(shin::ui::tr("Project history"), width, compact_diagnostics ? 16.0f : 18.0f, ink, 1, 54);
        if (!minimal) {
            ImGui::Spacing();
            const auto counts_origin = ImGui::GetCursorScreenPos();
            const auto requests = retained ? compact_number(static_cast<double>(count(*retained, "requests"))) : "—";
            const auto errors = retained ? compact_number(static_cast<double>(count(*retained, "errors"))) : "—";
            const auto failures = project.contains("failures") ? std::to_string(count(project, "failures")) : "—";
            if (!compact_diagnostics && ImGui::BeginTable("diagnostic-history-counts", 3, ImGuiTableFlags_SizingStretchSame)) {
                ImGui::TableNextColumn();
                metric(shin::ui::tr("Requests"), requests, ink, 23);
                ImGui::PushFont(nullptr, 11); ImGui::TextColored(muted, "%s", shin::ui::tr("Retained history")); ImGui::PopFont();
                ImGui::TableNextColumn();
                metric(shin::ui::tr("Response errors"), errors, muted, 23);
                ImGui::PushFont(nullptr, 11); ImGui::TextColored(muted, "%s", shin::ui::tr("Retained history")); ImGui::PopFont();
                ImGui::TableNextColumn();
                metric(shin::ui::tr("Check failures"), failures, muted, 23);
                ImGui::PushFont(nullptr, 11); ImGui::TextColored(muted, "%s", shin::ui::tr("This service run")); ImGui::PopFont();
                ImGui::EndTable();
            }
            if (compact_diagnostics) {
                ImGui::PushFont(nullptr, 13);
                ImGui::TextColored(ink, shin::ui::tr("Requests: %s · Errors: %s"), requests.c_str(), errors.c_str());
                ImGui::PopFont();
                ImGui::PushFont(nullptr, 11);
                ImGui::TextColored(muted, shin::ui::tr("Check failures this run: %s"), failures.c_str());
                ImGui::PopFont();
            }
            ui_actions.push_back({shin::ui::tr("Retained requests: ") + requests + shin::ui::tr(", response errors: ") + errors +
                shin::ui::tr(". Check failures this service run: ") + failures, selected, counts_origin,
                {width, ImGui::GetCursorScreenPos().y - counts_origin.y}, 55, true, true});
            ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
            diagnostic_text(diagnostic_history_status(project, view.connected), width, 14,
                view.connected && active_issue(project) ? amber : ink, 2, 56);
            if (footer - ImGui::GetCursorPosY() >= 40) {
                const auto line = prior_code.empty() ? shin::ui::tr("Historical counters do not imply a current error.") : shin::ui::tr("Last failure: ") + explain(prior_code).cause;
                diagnostic_text(line, width, 13, muted, compact_diagnostics ? 2 : 3, 57);
            }
        }
    }

    ImGui::SetCursorPosY(footer);
    ImGui::Separator();
    if (!diagnostic_history_first) {
        const auto origin = ImGui::GetCursorScreenPos();
#if defined(__APPLE__)
        const float action_width = width - 112;
#else
        const float action_width = width - 76;
#endif
        const bool review_action = current_code == "route-revision-conflict";
        if (ImGui::InvisibleButton(review_action ? "profile-review" : "project-history", {action_width, 28}, ImGuiButtonFlags_EnableNav))
            activate(review_action ? 70 : 34);
        const bool hovered = ImGui::IsItemHovered();
        if (hovered) ImGui::GetWindowDrawList()->AddRectFilled(origin, {origin.x + action_width, origin.y + 28}, IM_COL32(43,49,52,255), 4);
        if (ImGui::IsItemFocused()) ImGui::GetWindowDrawList()->AddRect(origin,
            {origin.x + action_width, origin.y + 28}, ImGui::ColorConvertFloat4ToU32(blue), 4);
        draw_icon(review_action ? Icon::details : Icon::history, {origin.x + 2, origin.y + 5}, 18, review_action ? amber : muted);
        ImGui::PushFont(nullptr, 13);
        ImGui::GetWindowDrawList()->AddText({origin.x + 27, origin.y + centered_item_y(28, 13)},
            ImGui::ColorConvertFloat4ToU32(ink), review_action ? shin::ui::tr("Review changes") : shin::ui::tr("Project history"));
        ImGui::PopFont();
        ui_actions.push_back({review_action ? shin::ui::tr("Review changes") : shin::ui::tr("Project history"), selected, origin,
            {action_width, 28}, review_action ? 70 : 34, true});
#if defined(__APPLE__)
        ImGui::SameLine(width - 102);
        const bool actions_enabled = view.connected && !diagnostic_root.empty() && !project_management_busy.load();
        icon_button(Icon::more, shin::ui::tr("Project actions"), 84, actions_enabled, {28, 28});
        ImGui::SameLine();
#else
        ImGui::SameLine(width - 66);
#endif
    } else ImGui::SetCursorPosX(width - 66);
    const bool copied = diagnostic_copied != Clock::time_point{} && Clock::now() - diagnostic_copied < std::chrono::seconds(2);
    icon_button(copied ? Icon::current : Icon::copy, copied ? shin::ui::tr("Diagnostics copied") : shin::ui::tr("Copy safe diagnostics"),
        36, !diagnostic_copy.empty(), {28, 28});
    ImGui::SameLine();
    icon_button(Icon::folder, shin::ui::tr("Show project folder"), 38, !diagnostic_root.empty(), {28, 28});
    (void)telemetry;
}
void render_projects(const ViewState& view, const Json& telemetry, const Json& history) {
    diagnostic_project.clear(); diagnostic_copy.clear(); diagnostic_root.clear();
    const auto projects = telemetry.value("project_states", Json::array());
    if (!selected.empty()) {
        for (const auto& p : projects) if (p.at("id") == selected) {
            render_project_diagnostics(view, p, telemetry, history);
            return;
        }
        diagnostic_arrow("missing-project-back", shin::ui::tr("Back to project list"), 32, ImGuiDir_Left, true);
        ImGui::TextWrapped("%s", shin::ui::tr("The project is missing from the latest service response. Its current state is unknown."));
        return;
    }
    const float width = ImGui::GetContentRegionAvail().x;
#if defined(__APPLE__)
    const bool project_busy = project_management_busy.load();
    ImGui::BeginDisabled(project_busy);
    if (ImGui::Button(project_busy ? shin::ui::tr("Operation in progress…") : shin::ui::tr("Add project folder…"), {width, 28})) activate(80);
    ui_actions.push_back({project_busy ? shin::ui::tr("Project operation in progress") : shin::ui::tr("Add project folder…"), "",
        ImGui::GetItemRectMin(), ImGui::GetItemRectSize(), 80, !project_busy});
    ImGui::EndDisabled();
#endif
    const auto layout = shin::ui::project_table_layout(ImGui::GetContentRegionAvail().y, static_cast<int>(projects.size()));
    project_page = std::clamp(project_page, 0, layout.pages - 1);
    const auto residents = view.data.is_object() ? view.data.value("resident_projects", Json::array()) : Json::array();
    const bool history_known = history.value("status", "") == "ready";
    ImGui::BeginChild("projects-table-region", {0, layout.table_height}, ImGuiChildFlags_None,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PushFont(nullptr, 12);
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, {5, 4});
    if (ImGui::BeginTable("projects-table", 4, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg |
        ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_NoSavedSettings)) {
        ImGui::TableSetupColumn(shin::ui::tr("Project"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("##state", ImGuiTableColumnFlags_WidthFixed, 20);
        ImGui::TableSetupColumn("RAM", ImGuiTableColumnFlags_WidthFixed, 42);
        ImGui::TableSetupColumn(shin::ui::tr("Requests"), ImGuiTableColumnFlags_WidthFixed, 48);
        ImGui::TableNextRow(ImGuiTableRowFlags_Headers, layout.header_height);
        for (int column = 0; column < 4; ++column) {
            ImGui::TableSetColumnIndex(column);
            ImGui::TableHeader(ImGui::TableGetColumnName(column));
            if (ImGui::IsItemHovered() && column == 2) ImGui::SetTooltip("%s", shin::ui::tr("Search worker RSS, MiB. Background tasks and Monitor are separate. No worker or sample: —."));
            if (ImGui::IsItemHovered() && column == 3) ImGui::SetTooltip("%s", shin::ui::tr("Retained requests, not an assessment of answer quality."));
        }
        int index = 0;
        for (const auto& p : projects) {
            if (index++ / layout.page_size != project_page) continue;
            const auto id = p.at("id").get<std::string>();
            const auto name = p.at("name").get<std::string>();
            const auto root = shin::ui::text_field(p, "root", 4096);
            const auto state = shin::ui::project_state(p, view.connected);
            const auto state_label = state == shin::ui::ProjectState::blocked
                ? shin::ui::diagnostic_overview(p, true).title : std::string(shin::ui::state_text(state));
            const auto elapsed = view.received == Clock::time_point{} ? 0 : std::chrono::duration<double, std::milli>(Clock::now() - view.received).count();
            const auto success = shin::ui::age_ms(p, "last_success_age_ms", elapsed);
            const auto age = shin::ui::tr("Successful: ") + shin::ui::age_text(success);
            const auto memory = shin::ui::resident_memory(residents, root, view.connected);
            const auto memory_text = memory ? number(*memory, "") : "—";
            std::uint64_t calls = 0;
            for (const auto& row : history.value("projects", Json::array()))
                if (row.at("id") == id) calls = shin::ui::count(row, "requests");
            const auto calls_text = history_known ? compact_number(static_cast<double>(calls)) : "—";
            ImGui::TableNextRow(ImGuiTableRowFlags_None, layout.row_height);
            ImGui::TableSetColumnIndex(0);
            const auto origin = ImGui::GetCursorScreenPos();
            const float name_width = ImGui::GetContentRegionAvail().x;
            if (ImGui::Selectable(("##project-" + id).c_str(), false, ImGuiSelectableFlags_SpanAllColumns,
                {0, layout.row_height - 8})) select_project(id);
            const bool hovered = ImGui::IsItemHovered();
            ImGui::GetWindowDrawList()->AddText({origin.x, origin.y + 1}, ImGui::ColorConvertFloat4ToU32(ink), clipped(name, name_width).c_str());
            ui_actions.push_back({name + ": " + state_label + "; " + age +
                "; RAM " + memory_text + shin::ui::tr(" MiB; requests: ") + calls_text, id, {origin.x - 5, origin.y - 4}, {width, layout.row_height}, -1});
            ImGui::TableSetColumnIndex(1);
            draw_icon(state_icon(state), ImGui::GetCursorScreenPos(), 17, project_color(state));
            const auto right_text = [](const std::string& value) {
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(value.c_str()).x));
                ImGui::TextColored(muted, "%s", value.c_str());
            };
            ImGui::TableSetColumnIndex(2); right_text(memory_text);
            ImGui::TableSetColumnIndex(3); right_text(calls_text);
            if (hovered) ImGui::SetTooltip(shin::ui::tr("%s\n%s\n%s\n%s\nRAM: %s MiB · Retained requests: %s\nRetained check failures: %llu"),
                name.c_str(), root.c_str(), state_label.c_str(), age.c_str(), memory_text.c_str(), calls_text.c_str(),
                static_cast<unsigned long long>(shin::ui::count(p, "failures")));
        }
        ImGui::EndTable();
    }
    ImGui::PopStyleVar();
    ImGui::PopFont();
    if (projects.empty()) ImGui::TextColored(muted, "%s", view.connected ? shin::ui::tr("No connected projects") : shin::ui::tr("Project list unavailable"));
    ImGui::EndChild();
    std::uint64_t requests = 0, errors = 0;
    for (const auto& row : history.value("projects", Json::array())) {
        requests += row.at("requests").get<std::uint64_t>(); errors += row.at("errors").get<std::uint64_t>();
    }
    icon_button(Icon::requests, shin::ui::tr("All retained requests"), 53, true, {80, 24},
        history_known ? compact_number(static_cast<double>(requests)) : "—", true);
    ImGui::SameLine();
    icon_button(Icon::history, shin::ui::tr("Historical errors: project explanation"), 37, !history_project.empty(), {58, 24},
        history_known ? std::to_string(errors) : "—");
    if (layout.pages > 1) {
        ImGui::SameLine(width - 100);
        diagnostic_arrow("previous-project-page", shin::ui::tr("Previous projects"), 30, ImGuiDir_Left, project_page > 0);
        ImGui::SameLine(); ImGui::TextColored(muted, "%d/%d", project_page + 1, layout.pages); ImGui::SameLine();
        diagnostic_arrow("next-project-page", shin::ui::tr("Next projects"), 31, ImGuiDir_Right, project_page + 1 < layout.pages);
    }
}

void configure_imgui() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    if (auto* font = io.Fonts->AddFontFromFileTTF(platform_font().c_str(), 13)) io.FontDefault = font;
    ImGui::StyleColorsDark();
    auto& style = ImGui::GetStyle();
    style.WindowPadding = {16, 14};
    style.WindowRounding = 0;
    style.FrameRounding = 4;
    style.ItemSpacing = {7, 5};
    style.Colors[ImGuiCol_WindowBg] = {0.091f, 0.106f, 0.11f, 1};
    style.Colors[ImGuiCol_ChildBg] = style.Colors[ImGuiCol_WindowBg];
    style.Colors[ImGuiCol_Text] = ink;
    style.Colors[ImGuiCol_CheckMark] = green;
    style.Colors[ImGuiCol_FrameBg] = {0.18f, 0.20f, 0.22f, 1};
}

void render(const ViewState& view) {
    ui_actions.clear();
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
    int attention = 0;
    for (const auto& p : projects) if (shin::ui::active_issue(p)) ++attention;
    const bool registry_problem = view.connected && (!data.value("registry_error", "").empty() ||
        !data.value("enrollment_errors", Json::object()).empty());
    const auto accent = !view.connected ? muted : attention || registry_problem ? amber : green;

    ImGui::SetNextWindowPos({0, 0});
    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
    ImGui::Begin("Shin", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    ImGui::PushFont(nullptr, 21); ImGui::TextUnformatted("Shin"); ImGui::PopFont();
    ImGui::SameLine(90); ImGui::SetNextItemWidth(130);
    std::array<std::string, 3> language_labels{shin::ui::tr("System"), "English", "Русский"};
    const bool language_pending = language_preference != applied_language_preference;
    if (language_pending) language_labels[static_cast<std::size_t>(language_preference)] += " *";
    const char* languages[]{language_labels[0].c_str(), language_labels[1].c_str(), language_labels[2].c_str()};
    if (ImGui::Combo("##ui-language", &language_preference, languages, 3)) {
        platform_save_language(language_preference); language_changed = Clock::now();
    }
    if (ImGui::IsItemHovered() || Clock::now() - language_changed < std::chrono::seconds(5))
        ImGui::SetTooltip("%s", shin::ui::tr(language_preference != applied_language_preference ?
            "Restart Monitor to apply the language." : "Language"));
    ImGui::SameLine(ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x - 20);
    status_badge(!view.connected ? Icon::offline : attention || registry_problem ? Icon::blocked : Icon::current,
        !view.connected ? shin::ui::tr("No fresh service response") : attention || registry_problem ?
        shin::ui::tr("Service connected; projects need attention") : shin::ui::tr("Service connected"), accent, 50, 20);
    ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
    if (!profile_review_open) {
        const float tab_width = (ImGui::GetContentRegionAvail().x - 14) / 3;
        view_tab(shin::ui::tr("Overview"), 10, dashboard_view == 0, tab_width); ImGui::SameLine();
        view_tab(shin::ui::tr("Projects"), 11, dashboard_view == 1, tab_width); ImGui::SameLine();
        view_tab(shin::ui::tr("Models"), 12, dashboard_view == 2, tab_width);
        ImGui::PushFont(nullptr, 11);
        if (!view.connected) ImGui::TextColored(amber, "%s", view.data.is_null() ? shin::ui::tr("Waiting for connection") : shin::ui::tr("Offline · last received data"));
        else if (attention) ImGui::TextColored(amber, shin::ui::tr("Context blocked: %d"), attention);
        else if (registry_problem) ImGui::TextColored(amber, "%s", shin::ui::tr("Project connection error"));
        else if (dashboard_view == 2) ImGui::TextColored(muted, shin::ui::tr("Connected projects: %zu of %zu"), shin::ui::mapped_model_projects(*view.usage, projects), projects.size());
        else ImGui::TextColored(muted, shin::ui::tr("Projects: %zu · Active requests: %llu"), projects.size(), data.value("query_workers", 0ULL));
        ImGui::PopFont();
    }
    const float footer_y = ImGui::GetWindowHeight() - 34;
    ImGui::BeginChild("view-content", {0, std::max(1.0f, footer_y - ImGui::GetCursorPosY() - 5)}, ImGuiChildFlags_None,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
#if defined(__APPLE__)
    if (projects.empty() && dashboard_view != 2 && !timing_open) {
        ImGui::TextWrapped("%s", shin::ui::tr("Choose a project to start using Shin. Setup also helps connect your coding agent."));
        ImGui::Spacing();
        if (ImGui::Button(shin::ui::tr("Set up Shin…"), {ImGui::GetContentRegionAvail().x, 32})) activate(85);
        ui_actions.push_back({shin::ui::tr("Set up Shin…"), "", ImGui::GetItemRectMin(), ImGui::GetItemRectSize(), 85, true});
    } else
#endif
    if (dashboard_view == 0 && timing_open) render_request_timing(view, telemetry);
    else if (dashboard_view == 0) render_overview(view, telemetry, history);
    else if (dashboard_view == 1) render_projects(view, telemetry, history);
    else render_models(view, telemetry, history);
    ImGui::EndChild();
    ImGui::SetCursorPosY(footer_y);
    ImGui::Separator();
    if (dashboard_view != 2) history_state(history, view.connected,
        telemetry.value("projection", Json::object()).value("truncated", false));
    else {
        ImGui::PushFont(nullptr, 11);
        if (view.usage->contains("projects")) {
            ImGui::TextColored(muted, shin::ui::tr("Unassigned: %llu · Skipped: %llu"), view.usage->value("unassigned_sessions", 0ULL), view.usage->value("skipped_sessions", 0ULL));
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", shin::ui::tr("Token counters are reported by clients; this is not a bill. Sessions from all clients in the selected period. Unassigned means no exact link to a connected project. Skipped history could not be read; reasons appear above the tokens. Connecting all projects does not imply complete history. Cache and reasoning counters may overlap other counters; they are not added together."));
        } else ImGui::TextColored(muted, "%s", shin::ui::tr("Model history not connected"));
        ImGui::PopFont();
    }
    ImGui::End();
}
} // namespace
