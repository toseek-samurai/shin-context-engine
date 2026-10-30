#include "shin/observer/local_service.h"
#include <cerrno>
#include <iostream>
#include <sys/resource.h>
#include <sys/wait.h>

int main(int argc, char** argv) {
    using namespace shin::observer;
    if (argc < 4) return 1;
    try {
        Json results = Json::array();
        const bool cancel = std::string(argv[3]) == "--cancel";
        const bool wrong_identity = std::string(argv[3]) == "--wrong-identity";
        const bool first_check = std::string(argv[3]) == "--first-watch-check";
        const bool marker_plan = std::string(argv[3]) == "--marker-plan";
        for (int i = cancel || wrong_identity || first_check || marker_plan ? 4 : 3; i < argc; ++i) {
            LocalProjectRequest request;
            request.command = "status"; request.home = argv[2]; request.root = argv[i];
            if (marker_plan) {
                const auto plan = prepare_marker_generator_transition(request);
                results.push_back({{"root_hash", digest(fs::canonical(request.root).string())},
                    {"transition_required", !plan.is_null()}});
                continue;
            }
            if (wrong_identity) request.maintenance_root_identity = "retired-registry-identity";
            int checks = 0;
            if (first_check) {
                bool checked = false;
                (void)watch_local_projects(request, {request.root}, 1, [] { return false; }, {}, {},
                    [&](const fs::path&, const Json& state) {
                        if (!checked && state.at("phase") == "checking" && state.at("ticks") == 0) {
                            checked = true;
                            results.push_back(qualify_local_project(request, argv[1]));
                        }
                    });
                if (!checked) throw Error("test-check-not-observed", "First controller check was not observed.");
            } else results.push_back(qualify_local_project(request, argv[1], [&] { return cancel && ++checks > 1; }));
            if (::waitpid(-1, nullptr, WNOHANG) != -1 || errno != ECHILD)
                throw Error("test-child-leaked", "Qualification did not reap its worker.");
        }
        rusage self{}, children{};
        if (::getrusage(RUSAGE_SELF, &self) || ::getrusage(RUSAGE_CHILDREN, &children)) return 1;
        std::cout << Json{{"ok", true}, {"results", results},
            {"parent_maxrss", self.ru_maxrss}, {"child_maxrss", children.ru_maxrss}}.dump() << '\n';
        return 0;
    } catch (const Error& error) {
        if (::waitpid(-1, nullptr, WNOHANG) != -1 || errno != ECHILD) return 3;
        std::cout << Json{{"ok", false}, {"code", error.code}}.dump() << '\n'; return 2;
    }
}
