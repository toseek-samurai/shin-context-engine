#pragma once
#include "shin/observer/local_project.h"
#include "updates.h"
#include <chrono>
#include <array>

namespace shin::observer::tui {
using Clock = std::chrono::steady_clock;
struct Snapshot {
    Json service, storage, usage;
    bool connected = false, storage_loading = false;
    std::string error = "connecting", storage_error;
    Clock::time_point received{};
    Updates updates;
};
enum class Tone { normal, heading, good, warning, selected, muted, info, error, idle };
struct Line { std::string text; Tone tone = Tone::normal; };
struct State {
    int tab = 0, scroll = 0, traffic = 0, metric = 0, range = 0;
    bool detail = false, retained = true, help = false;
    std::string project;
    std::string update_confirmation;
    std::size_t usage_project = 0;
    int activity_page = 2, settings_page = 7;
};
// Leaf pages keep their existing data/render ownership; navigation groups them.
int main_section(const State& state);
std::array<int, 3> subpages(const State& state);
void show_page(State& state, int page);
void select_section(State& state, int section);
void move_subpage(State& state, int direction);
// Presentation only. Never queries, collects usage, enrolls or changes state.
std::vector<Line> lines(const Snapshot& snapshot, State& state);
void select_project(const Snapshot& snapshot, State& state, int direction);
std::string terminal_text(std::string_view text, int columns, bool pad = false);
bool fresh(const Snapshot& snapshot);
Line project_status(const Json& project, bool connected);
int run(const LocalProjectRequest& request, const fs::path& usage_file, std::string_view language,
        UpdateOperation updates = {});
} // namespace shin::observer::tui
