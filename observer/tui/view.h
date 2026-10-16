#pragma once
#include "shin/observer/local_project.h"
#include <chrono>

namespace shin::observer::tui {
using Clock = std::chrono::steady_clock;
struct Snapshot {
    Json service, storage, usage;
    bool connected = false, storage_loading = false;
    std::string error = "connecting", storage_error;
    Clock::time_point received{};
};
enum class Tone { normal, heading, good, warning, selected };
struct Line { std::string text; Tone tone = Tone::normal; };
struct State {
    int tab = 0, scroll = 0, traffic = 0;
    bool detail = false, retained = true, help = false;
    std::string project;
    std::size_t usage_project = 0;
};
// Presentation only. Never queries, collects usage, enrolls or changes state.
std::vector<Line> lines(const Snapshot& snapshot, State& state);
void select_project(const Snapshot& snapshot, State& state, int direction);
std::string terminal_text(std::string_view text, int columns, bool pad = false);
int run(const LocalProjectRequest& request, const fs::path& usage_file, std::string_view language);
} // namespace shin::observer::tui
