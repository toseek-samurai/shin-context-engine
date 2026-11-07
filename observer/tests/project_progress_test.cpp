#include "../ui/project_progress.h"
#include <iostream>
#include <stdexcept>
#include <vector>

int main() try {
    const auto check = [](bool value) { if (!value) throw std::runtime_error("project progress contract failed"); };
    shin::ui::ProjectProgressReader reader;
    std::vector<int> stages;
    const auto report = [&](int stage) { stages.push_back(stage); };
    const auto frame = [](const char* stage) {
        return nlohmann::json{{"schema_version", "shin.project-progress.v1"}, {"stage", stage}}.dump() + "\n";
    };
    const auto first = frame("checking-project");
    reader.feed(first.substr(0, 17), report);
    check(stages.empty());
    reader.feed(first.substr(17), report);
    reader.feed(frame("candidate-build") + frame("source-cache"), report);
    reader.feed(frame("source-cache") + frame("candidate-validation"), report);
    check(stages == std::vector<int>({0, 1, 3})); // No repeated or backward notifications.
    reader.feed(frame("untrusted-stage"), report);
    reader.feed("{\"schema_version\":17,\"stage\":false}\n", report);
    reader.feed("{\"ok\":false,\"error\":{\"code\":\"fixture-error\"}}", report);
    const auto errors = reader.finish();
    check(errors.find("fixture-error") != std::string::npos && errors.find("untrusted-stage") != std::string::npos);
    check(errors.find("candidate-build") == std::string::npos);
    const auto english = std::string(shin::ui::project_connection_label(3));
    shin::ui::set_language(shin::ui::Language::ru);
    check(english != shin::ui::project_connection_label(3));
    std::cout << "Project progress: fragmented frames, forward-only stages, retained errors and EN/RU passed\n";
} catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
