#include "../ui/update_progress.h"
#include <iostream>
#include <stdexcept>

using namespace shin::observer;
int main(int argc, char** argv) try {
    const auto check = [](bool value) { if (!value) throw std::runtime_error("update progress contract failed"); };
    UpdateTimeline timeline;
    timeline.observe({ReleaseUpdateStep::downloading, 5, 10});
    timeline.observe({ReleaseUpdateStep::verifying_archive});
    timeline.observe({ReleaseUpdateStep::starting_service});
    check(timeline.done[1] && timeline.done[2] && !timeline.done[3] && timeline.seen[9]);
    timeline.observe({ReleaseUpdateStep::rolling_back});
    timeline.observe({ReleaseUpdateStep::failed});
    check(!timeline.done[9]); // A failed or rolled-back stage is not complete.
    check(!parse_update_progress(Json{{"step", 999U}, {"received", 0U}, {"total", 0U}}));
    check(!parse_update_progress(Json{{"step", 2U}, {"received", -1}, {"total", 0U}}));
    check(!parse_update_progress(Json{{"step", 2U}, {"received", 0U}, {"total", "wrong"}}));
    check(parse_update_progress(update_progress_frame({ReleaseUpdateStep::checking_projects, 2, 3}))->received == 2);
    report_update([](auto) { throw std::runtime_error("unavailable observer"); }, {ReleaseUpdateStep::staging});
    ReleaseUpdateProgressSink retired;
    {
        TerminalUpdateProgress terminal(false, false, false);
        retired = terminal.sink();
    }
    retired({ReleaseUpdateStep::starting_service}); // Late network delivery has no presenter to access.
    if (argc > 1) {
        TerminalUpdateProgress terminal(std::string_view(argv[1]) == "force", std::string_view(argv[1]) == "json");
        terminal.report({ReleaseUpdateStep::downloading, 5 * 1048576, 10 * 1048576});
        terminal.report({ReleaseUpdateStep::starting_service});
        std::this_thread::sleep_for(std::chrono::milliseconds(1250));
        terminal.report({ReleaseUpdateStep::checking_projects, 2, 3});
        terminal.report({ReleaseUpdateStep::completed});
        std::cout << "{\"ok\":true}\n";
    }
} catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
