#include "shin/observer/local_package.h"
#include <fstream>
#include <iostream>

int main(int argc, char** argv) {
    using namespace shin::observer;
    if (argc != 2) return 1;
    try {
        const auto base = fs::absolute(argv[1]);
        const auto first = base / "Cellar/shin/1", second = base / "Cellar/shin/2", opt = base / "opt/shin";
        for (const auto& keg : {first, second}) {
            fs::create_directories(keg / "bin");
            std::ofstream(keg / "bin/shin-ce") << "fixture";
        }
        fs::create_directories(opt.parent_path()); fs::create_directory_symlink(first, opt);
        const auto app = first / "Shin Monitor.app/Contents/MacOS/Shin Monitor";
        fs::create_directories(app.parent_path()); std::ofstream(app) << "fixture";
        if (stable_package_path(app, opt, "bin/shin-ce") != opt / "bin/shin-ce") return 7;
        if (stable_package_path(first / "bin/shin-ce", opt, "bin/shin-mcp") != opt / "bin/shin-mcp") return 2;
        fs::remove(opt); fs::create_directory_symlink(second, opt);
        try { (void)stable_package_path(first / "bin/shin-ce", opt, "bin/shin-mcp"); return 3; }
        catch (const Error& error) { if (error.code != "package-generation-changed") throw; }
        if (stable_package_path(second / "bin/shin-ce", opt, "share/shin-context/profiles") != opt / "share/shin-context/profiles") return 4;
        for (const auto& relative : {fs::path("../outside"), fs::path("/outside"), fs::path("bin/../outside")}) {
            try { (void)stable_package_path(second / "bin/shin-ce", opt, relative); return 5; }
            catch (const Error& error) { if (error.code != "package-prefix-invalid") throw; }
        }
        std::cout << "Stable package locators preserve aliases and refuse stale generations/traversal\n";
        fs::remove_all(base); return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 6; }
}
