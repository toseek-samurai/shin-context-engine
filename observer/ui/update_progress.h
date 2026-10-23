#pragma once
#include "shin/observer/update_progress.h"
#include "language.h"
#include <chrono>
#include <cstdio>
#include <mutex>
#include <span>
#include <thread>
#include <unistd.h>
#ifdef __APPLE__
#include <fcntl.h>
#endif

namespace shin::observer {
inline const char* update_stage_key(ReleaseUpdateStep step) {
    switch (step) {
        case ReleaseUpdateStep::preparing: return "Preparing the update…";
        case ReleaseUpdateStep::checking_release: return "Checking the reviewed release…";
        case ReleaseUpdateStep::waiting_for_monitor: return "Closing Shin Monitor…";
        case ReleaseUpdateStep::downloading: return "Downloading the update…";
        case ReleaseUpdateStep::verifying_archive: return "Verifying the download…";
        case ReleaseUpdateStep::extracting: return "Unpacking the update…";
        case ReleaseUpdateStep::verifying_kit: return "Checking the new kit…";
        case ReleaseUpdateStep::qualifying_projects: return "Checking project compatibility…";
        case ReleaseUpdateStep::staging: return "Staging the new kit…";
        case ReleaseUpdateStep::stopping_service: return "Stopping the current service…";
        case ReleaseUpdateStep::switching_kit: return "Switching to the new kit…";
        case ReleaseUpdateStep::starting_service: return "Starting Shin and admitting projects…";
        case ReleaseUpdateStep::checking_projects: return "Verifying project readiness…";
        case ReleaseUpdateStep::restoring_service_state: return "Restoring the service preference…";
        case ReleaseUpdateStep::rolling_back: return "Restoring the previous kit and service…";
        case ReleaseUpdateStep::confirming: return "Confirming the installed version…";
        case ReleaseUpdateStep::completed: return "Update completed";
        case ReleaseUpdateStep::failed: return "Update needs review";
        case ReleaseUpdateStep::installing: return "Installing and restarting Shin…";
    }
    return "Preparing the update…";
}
inline std::string update_detail(ReleaseUpdateProgress value) {
    char bytes[160];
    if (value.step == ReleaseUpdateStep::downloading || value.step == ReleaseUpdateStep::staging) {
        if (value.total) std::snprintf(bytes, sizeof(bytes), shin::ui::tr("%.1f / %.1f MiB · %.0f%%"),
            value.received / 1048576.0, value.total / 1048576.0,
            static_cast<double>(std::min(100.0L, 100.0L * value.received / value.total)));
        else std::snprintf(bytes, sizeof(bytes), shin::ui::tr("%.1f MiB"), value.received / 1048576.0);
        return shin::ui::decimal(bytes);
    }
    if (value.step == ReleaseUpdateStep::qualifying_projects || value.step == ReleaseUpdateStep::checking_projects) {
        std::snprintf(bytes, sizeof(bytes), shin::ui::tr("Projects checked: %llu / %llu"),
            static_cast<unsigned long long>(value.received), static_cast<unsigned long long>(value.total));
        return bytes;
    }
    return {};
}
// stderr is a presenter only; stdout remains the native JSON receipt.
// A redirected invocation stays silent unless --progress is explicitly selected.
class TerminalUpdateProgress {
    std::mutex mutex_;
    bool enabled_, tty_, json_, release_, reported_ = false;
    ReleaseUpdateProgress current_;
    std::chrono::steady_clock::time_point started_, written_{};
    std::jthread heartbeat_;
    void draw() noexcept {
        if (!enabled_ || !reported_) return;
        try {
            const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - started_).count();
            static constexpr std::array local_steps{ReleaseUpdateStep::preparing, ReleaseUpdateStep::verifying_kit,
                ReleaseUpdateStep::qualifying_projects, ReleaseUpdateStep::staging, ReleaseUpdateStep::stopping_service,
                ReleaseUpdateStep::switching_kit, ReleaseUpdateStep::starting_service, ReleaseUpdateStep::checking_projects,
                ReleaseUpdateStep::confirming};
            const auto plan = release_ ? std::span<const ReleaseUpdateStep>(update_steps) : std::span<const ReleaseUpdateStep>(local_steps);
            auto step = current_.step;
            if(step == ReleaseUpdateStep::checking_release || step == ReleaseUpdateStep::waiting_for_monitor) step = ReleaseUpdateStep::preparing;
            if(step == ReleaseUpdateStep::restoring_service_state) step = ReleaseUpdateStep::checking_projects;
            const auto index = static_cast<std::size_t>(std::find(plan.begin(), plan.end(), step) - plan.begin());
            const auto detail = update_detail(current_);
            if (tty_) std::fprintf(stderr, "\r%c[2K", 27);
            if (index < plan.size()) std::fprintf(stderr, "[%zu/%zu] ", index + 1, plan.size());
            std::fprintf(stderr, "%s%s%s · %lld%s%s", shin::ui::tr(update_stage_key(current_.step)),
                detail.empty() ? "" : "  ", detail.c_str(), static_cast<long long>(seconds), shin::ui::tr(" s"), tty_ ? "" : "\n");
            std::fflush(stderr);
        } catch (...) { /* Presentation cannot change the native result. */ }
    }
public:
    TerminalUpdateProgress(bool force = false, bool json = false, bool allowed = true, bool release = true)
        : enabled_(allowed && (force || json || ::isatty(STDERR_FILENO))),
          tty_(enabled_ && !json && ::isatty(STDERR_FILENO) && std::string_view(std::getenv("TERM") ? std::getenv("TERM") : "") != "dumb"), json_(json), release_(release) {
#ifdef __APPLE__
        // A helper/viewer disappearing must not terminate the native updater.
        if (enabled_) (void)::fcntl(STDERR_FILENO, F_SETNOSIGPIPE, 1);
#endif
        if (enabled_ && !json_) shin::ui::set_language(shin::ui::terminal_language("auto"));
        if (tty_) heartbeat_ = std::jthread([this](std::stop_token stop) {
            while (!stop.stop_requested()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                std::lock_guard lock(mutex_);
                if (std::chrono::steady_clock::now() - written_ < std::chrono::seconds(1)) continue;
                draw(); written_ = std::chrono::steady_clock::now();
            }
        });
    }
    void report(ReleaseUpdateProgress value) noexcept {
        if (!enabled_) return;
        std::lock_guard lock(mutex_);
        const auto now = std::chrono::steady_clock::now();
        const bool changed = !reported_ || value.step != current_.step;
        if (changed) { if (reported_ && tty_) std::fputc('\n', stderr); started_ = now; }
        current_ = value; reported_ = true;
        if (!changed && value.received < value.total && now - written_ < std::chrono::milliseconds(json_ ? 200 : 1000)) return;
        if (json_) {
            try { const auto frame = update_progress_frame(value).dump() + "\n"; std::fputs(frame.c_str(), stderr); std::fflush(stderr); } catch (...) {}
        } else draw();
        written_ = now;
    }
    ReleaseUpdateProgressSink sink() { return [this](auto value) { report(value); }; }
    ~TerminalUpdateProgress() {
        if (heartbeat_.joinable()) { heartbeat_.request_stop(); heartbeat_.join(); }
        if (reported_ && tty_) std::fputc('\n', stderr);
    }
};
} // namespace shin::observer
