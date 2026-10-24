#include "sand.h"
#include "../ui/language.h"
#include "shin/observer/local_service.h"
#include "shin/observer/local_supervision.h"
#include "shin/observer/model_usage.h"
#include <algorithm>
#include <array>
#include <cerrno>
#include <clocale>
#include <condition_variable>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <mutex>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <termios.h>
#include <thread>
#include <unistd.h>
#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/screen/terminal.hpp>

extern char** environ;
namespace shin::observer::tui {
namespace {
volatile std::sig_atomic_t interrupted = 0;
void interrupt(int signal) { interrupted = signal; }
struct Terminal {
    termios original{};
    bool ascii = false, monochrome = false;
    Terminal() {
        const char* term = nullptr;
        for (auto** item = environ; item && *item; ++item) {
            if (std::string_view(*item).starts_with("TERM=")) term = *item + 5;
            if (std::string_view(*item).starts_with("NO_COLOR=")) monochrome = *(*item + 9) != '\0';
        }
        if (!::isatty(STDIN_FILENO) || !::isatty(STDOUT_FILENO) || !term || !*term || std::string_view(term) == "dumb")
            throw Error("tui-terminal-required", "Use shin-ce stats in an interactive terminal; use service-status --text in pipes.");
        if (::tcgetattr(STDIN_FILENO, &original) != 0)
            throw Error("tui-terminal-unavailable", "Cannot read terminal settings.");
        ascii = std::string_view(term) == "vt100" || std::string_view(term) == "ansi";
        // One-time CLI startup, before either poller thread exists. No later
        // locale changes; multibyte conversion runs only on this terminal thread.
        std::setlocale(LC_CTYPE, ""); // NOLINT(concurrency-mt-unsafe)
    }
    ~Terminal() {
        ::tcsetattr(STDIN_FILENO, TCSANOW, &original);
    }
};
struct Signals {
    static constexpr std::array numbers{SIGINT, SIGTERM, SIGHUP, SIGQUIT};
    std::array<struct sigaction, numbers.size()> previous{};
    Signals() {
        interrupted = 0;
        struct sigaction handler{}; handler.sa_handler = interrupt; (void)sigemptyset(&handler.sa_mask);
        for (std::size_t i = 0; i < numbers.size(); ++i) ::sigaction(numbers[i], &handler, &previous[i]);
    }
    ~Signals() { for (std::size_t i = 0; i < numbers.size(); ++i) ::sigaction(numbers[i], &previous[i], nullptr); }
};

// Storage inventory can touch many files. Keep the existing CLI ownership and
// validation path, in an owned, bounded child that can be cancelled on exit.
Json storage_snapshot(const fs::path& home, std::stop_token stop) {
    struct Child {
        int read = -1, write = -1;
        pid_t pid = -1;
        ~Child() {
            if (read >= 0) ::close(read);
            if (write >= 0) ::close(write);
            if (pid > 0) { ::kill(pid, SIGKILL); while (::waitpid(pid, nullptr, 0) < 0 && errno == EINTR) {} }
        }
    } child;
    int pipes[2];
    if (::pipe(pipes) != 0) throw Error("storage-unavailable", "Cannot create inventory channel.");
    child.read = pipes[0]; child.write = pipes[1];
    if (::fcntl(child.read, F_SETFD, FD_CLOEXEC) < 0 || ::fcntl(child.write, F_SETFD, FD_CLOEXEC) < 0 ||
        ::fcntl(child.read, F_SETFL, O_NONBLOCK) < 0) throw Error("storage-unavailable", "Cannot configure inventory channel.");
    struct Actions {
        posix_spawn_file_actions_t value;
        Actions() { if (::posix_spawn_file_actions_init(&value)) throw Error("storage-unavailable", "Cannot create inventory process."); }
        ~Actions() { ::posix_spawn_file_actions_destroy(&value); }
    } actions;
    if (::posix_spawn_file_actions_addopen(&actions.value, STDIN_FILENO, "/dev/null", O_RDONLY, 0) ||
        ::posix_spawn_file_actions_adddup2(&actions.value, child.write, STDOUT_FILENO) ||
        ::posix_spawn_file_actions_adddup2(&actions.value, child.write, STDERR_FILENO) ||
        ::posix_spawn_file_actions_addclose(&actions.value, child.read) ||
        ::posix_spawn_file_actions_addclose(&actions.value, child.write))
        throw Error("storage-unavailable", "Cannot configure inventory process.");
    std::array<std::string, 4> arguments{current_executable_path().string(), "storage-status", "--home", home.string()};
    std::array<char*, 5> argv{};
    for (std::size_t i = 0; i < arguments.size(); ++i) argv[i] = arguments[i].data();
    if (::posix_spawn(&child.pid, arguments[0].c_str(), &actions.value, nullptr, argv.data(), environ)) {
        child.pid = -1; throw Error("storage-unavailable", "Cannot start inventory process.");
    }
    ::close(child.write); child.write = -1;
    const auto deadline = Clock::now() + std::chrono::seconds(10);
    std::string output;
    std::array<char, 4096> buffer{};
    while (!stop.stop_requested() && Clock::now() < deadline) {
        pollfd fd{child.read, POLLIN, 0};
        if (::poll(&fd, 1, 50) < 0 && errno != EINTR) break;
        const auto count = ::read(child.read, buffer.data(), buffer.size());
        if (count > 0) {
            output.append(buffer.data(), static_cast<std::size_t>(count));
            if (output.size() > 65536) throw Error("storage-output-limit", "Storage inventory exceeds its output limit.");
        } else if (count == 0) {
            auto result = parse_json(output);
            if (result.value("ok", false) != true)
                throw Error(result.at("error").at("code").get<std::string>(), "Storage inventory is unavailable.");
            if (result.value("schema_version", "") != "shin.local-storage-status.v1")
                throw Error("storage-invalid", "Unexpected storage response.");
            return result;
        } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) break;
    }
    throw Error(stop.stop_requested() ? "storage-cancelled" : "storage-timeout", "Storage inventory stopped.");
}

class Poller {
public:
    Poller(LocalProjectRequest request, fs::path usage) : request_(std::move(request)), usage_(std::move(usage)),
        worker_([this](std::stop_token stop) { poll(stop); }),
        storage_worker_([this](std::stop_token stop) { poll_storage(stop); }) {}
    ~Poller() { worker_.request_stop(); storage_worker_.request_stop(); changed_.notify_all(); }
    void refresh(bool storage) {
        std::lock_guard lock(mutex_); ++revision_;
        if (storage) { storage_requested_ = true; state_.storage_loading = true; }
        changed_.notify_all();
    }
    Snapshot snapshot() { std::lock_guard lock(mutex_); return state_; }
private:
    void poll(std::stop_token stop) {
        auto usage_due = Clock::time_point{};
        std::size_t usage_revision = 0;
        while (!stop.stop_requested()) {
            std::size_t revision;
            { std::lock_guard lock(mutex_); revision = revision_; }
            try {
                request_.command = "service-status";
                const auto deadline = Clock::now() + std::chrono::seconds(2);
                auto result = request_local_service(request_, [&] { return stop.stop_requested() || Clock::now() >= deadline; });
                const auto& telemetry = result.at("telemetry");
                const auto& rows = telemetry.at("project_states");
                if (telemetry.at("schema_version") != "shin.local-service-telemetry.v1" || !rows.is_array() || rows.size() > 16 ||
                    !std::all_of(rows.begin(), rows.end(), [](const auto& row) { return row.is_object(); }))
                    throw Error("telemetry-invalid", "Unexpected telemetry contract.");
                std::lock_guard lock(mutex_);
                state_.service = std::move(result); state_.received = Clock::now(); state_.connected = true; state_.error.clear();
            } catch (const Error& error) {
                std::lock_guard lock(mutex_); state_.connected = false; state_.error = error.code;
            } catch (...) {
                std::lock_guard lock(mutex_); state_.connected = false; state_.error = "service-status-unavailable";
            }
            if ((Clock::now() >= usage_due || revision != usage_revision) && !stop.stop_requested()) {
                try {
                    auto value = read_model_usage(usage_.empty() ? request_.home / "usage" / "state.json" : usage_, now_ms());
                    std::lock_guard lock(mutex_); state_.usage = std::move(value);
                } catch (...) {
                    std::lock_guard lock(mutex_); state_.usage = {{"status", "unavailable"}};
                }
                usage_due = Clock::now() + std::chrono::seconds(30);
                usage_revision = revision;
            }
            std::unique_lock lock(mutex_);
            changed_.wait_for(lock, stop, std::chrono::seconds(2), [&] { return revision_ != revision; });
        }
    }
    void poll_storage(std::stop_token stop) {
        while (!stop.stop_requested()) {
            {
                std::unique_lock lock(mutex_);
                changed_.wait(lock, stop, [&] { return storage_requested_; });
                if (stop.stop_requested()) return;
                storage_requested_ = false;
            }
            try {
                auto value = storage_snapshot(request_.home, stop);
                std::lock_guard lock(mutex_); state_.storage = std::move(value); state_.storage_error.clear();
            } catch (const Error& error) {
                std::lock_guard lock(mutex_); state_.storage = nullptr; state_.storage_error = error.code;
            } catch (...) {
                std::lock_guard lock(mutex_); state_.storage = nullptr; state_.storage_error = "storage-unavailable";
            }
            std::lock_guard lock(mutex_); state_.storage_loading = storage_requested_;
        }
    }
    LocalProjectRequest request_;
    fs::path usage_;
    std::mutex mutex_;
    std::condition_variable_any changed_;
    Snapshot state_;
    std::size_t revision_ = 0;
    bool storage_requested_ = false;
    std::jthread worker_;
    std::jthread storage_worker_;
};

}

int run(const LocalProjectRequest& request, const fs::path& usage_file, std::string_view language) {
    ui::set_language(ui::terminal_language(language));
    Terminal terminal;
    Signals signals;
    Poller poller(request, usage_file);
    State state;
    HitTargets hits;
    auto screen = ftxui::ScreenInteractive::Fullscreen();
    screen.HandlePipedInput(false);
    screen.ForceHandleCtrlC(true);
    auto component = ftxui::Renderer([&] {
        const auto dimensions = ftxui::Terminal::Size();
        return sand_view(poller.snapshot(), state, dimensions.dimx, dimensions.dimy, hits, terminal.ascii, terminal.monochrome);
    });
    component = ftxui::CatchEvent(component, [&](ftxui::Event event) {
        using ftxui::Event;
        const auto snapshot = poller.snapshot();
        const int prior = state.tab;
        bool handled = true;
        const auto toggle_language = [] {
            ui::set_language(ui::language() == ui::Language::en ? ui::Language::ru : ui::Language::en);
        };
        if (event == Event::q || event == Event::Q || event == Event::CtrlD) screen.Exit();
        else if (event == Event::l || event == Event::L) toggle_language();
        else if (event == Event::CtrlL) screen.Clear();
        else if (event.is_character() && event.character().size() == 1 && event.character()[0] >= '1' && event.character()[0] <= '5') {
            state.tab = event.character()[0] - '1'; state.detail = false; state.help = false; state.scroll = 0;
        }
        else if (event == Event::Tab || event == Event::ArrowRight) state.tab = (state.tab + 1) % 5;
        else if (event == Event::TabReverse || event == Event::ArrowLeft) state.tab = (state.tab + 4) % 5;
        else if (event == Event::Character('?')) { state.help = !state.help; state.scroll = 0; }
        else if (event == Event::Escape || event == Event::Backspace) { state.detail = false; state.help = false; state.scroll = 0; }
        else if (event == Event::Return && (state.tab == 0 || state.tab == 1)) {
            state.tab = 1; state.detail = true; state.scroll = 0;
        } else if (event == Event::ArrowDown || event == Event::j || event == Event::ArrowUp || event == Event::k) {
            const int direction = event == Event::ArrowUp || event == Event::k ? -1 : 1;
            if ((state.tab == 0 || state.tab == 1) && !state.detail && !state.help) select_project(snapshot, state, direction);
            else state.scroll += direction;
        } else if (event == Event::PageDown || event == Event::PageUp)
            state.scroll += (event == Event::PageDown ? 1 : -1) * std::max(1, ftxui::Terminal::Size().dimy - 8);
        else if (event == Event::t && state.tab == 2) state.retained = !state.retained;
        else if (event == Event::f && state.tab == 2) state.traffic = (state.traffic + 1) % 3;
        else if (event == Event::p && state.tab == 4) { ++state.usage_project; state.scroll = 0; }
        else if (event == Event::g && state.tab == 0) state.metric = (state.metric + 1) % 3;
        else if (event == Event::h && state.tab == 0) state.range = (state.range + 1) % 3;
        else if (event.is_mouse()) {
            const auto& mouse = event.mouse();
            if (mouse.button == ftxui::Mouse::WheelUp || mouse.button == ftxui::Mouse::WheelDown) {
                const int direction = mouse.button == ftxui::Mouse::WheelUp ? -1 : 1;
                if ((state.tab == 0 || state.tab == 1) && !state.detail && !state.help) select_project(snapshot, state, direction);
                else state.scroll += direction * 3;
            }
            else if (mouse.button == ftxui::Mouse::Left && mouse.motion == ftxui::Mouse::Pressed) {
                const auto contains = [&](const ftxui::Box& box) { return box.Contain(mouse.x, mouse.y); };
                if (contains(hits.language)) toggle_language();
                if (contains(hits.back)) { state.detail = false; state.scroll = 0; }
                for (std::size_t i = 0; i < hits.tabs.size(); ++i) if (contains(hits.tabs[i])) {
                    state.tab = static_cast<int>(i); state.detail = false; state.help = false; state.scroll = 0;
                }
                for (std::size_t i = 0; i < hits.metrics.size(); ++i) if (contains(hits.metrics[i])) state.metric = static_cast<int>(i);
                for (std::size_t i = 0; i < hits.ranges.size(); ++i) if (contains(hits.ranges[i])) state.range = static_cast<int>(i);
                for (std::size_t i = 0; i < hits.project_count; ++i) if (contains(hits.projects[i])) {
                    state.project = hits.roots[i]; state.tab = 1; state.detail = true; state.scroll = 0;
                }
            } else handled = false;
        } else if (event != Event::r) handled = false;
        if (state.tab != prior) {
            state.scroll = 0; state.help = false;
            if (event != Event::Return && !event.is_mouse()) state.detail = false;
        }
        if (event == Event::r || (state.tab != prior && state.tab == 3)) poller.refresh(state.tab == 3);
        return handled;
    });
    // UI ages change between service observations. A bounded, low-rate wakeup
    // keeps them honest without running IPC or collectors on the render thread.
    std::jthread repaint([&](std::stop_token stop) {
        std::mutex mutex;
        std::condition_variable_any changed;
        std::unique_lock lock(mutex);
        while (!stop.stop_requested()) {
            screen.PostEvent(ftxui::Event::Custom);
            changed.wait_for(lock, stop, std::chrono::milliseconds(500), [] { return false; });
        }
    });
    screen.Loop(component);
    repaint.request_stop();
    return interrupted ? 128 + interrupted : 0;
}
} // namespace shin::observer::tui
