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
        for (int i = cancel || wrong_identity ? 4 : 3; i < argc; ++i) {
            LocalProjectRequest request;
            request.command = "status"; request.home = argv[2]; request.root = argv[i];
            if (wrong_identity) request.maintenance_root_identity = "retired-registry-identity";
            int checks = 0;
            results.push_back(qualify_local_project(request, argv[1], [&] { return cancel && ++checks > 1; }));
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
