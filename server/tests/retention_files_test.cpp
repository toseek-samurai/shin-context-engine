#include "shin/server/retention_files.h"
#include "shin/server/candidate_artifact.h"
#include "shin/server/route_files.h"

#include <fstream>
#include <iostream>
#include <sys/stat.h>
#include <unistd.h>

namespace {
using namespace shin::server;
namespace fs = std::filesystem;

void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
template <typename Function>
void rejects(const char* code, Function action) {
    try { action(); }
    catch (const ServiceError& error) {
        check(error.code == code, "wrong retention failure code");
        return;
    }
    throw std::runtime_error("unsafe retention operation succeeded");
}

class Fixture {
public:
    Fixture() {
        auto name = (fs::temp_directory_path() / "shin-retention-XXXXXX").string();
        check(::mkdtemp(name.data()) != nullptr, "temporary root failed");
        state = fs::canonical(name);
        root = state / candidate_artifact_name(1, std::string(64, 'a'));
        fs::create_directories(root / "docs/kb/generated");
        for (const auto& directory : {root, root / "docs", root / "docs/kb", root / "docs/kb/generated"})
            check(::chmod(directory.c_str(), 0700) == 0, "private directory failed");
        payload = root / "docs/kb/generated/documents.jsonl";
        write(payload, "one\ntwo\n");
    }
    ~Fixture() { std::error_code error; fs::remove_all(state, error); }
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;
    static void write(const fs::path& path, const std::string& value) {
        std::ofstream output(path);
        output << value;
        check(output.good(), "fixture write failed");
        check(::chmod(path.c_str(), 0600) == 0, "private file failed");
    }
    void remove(const Json& inventory) { remove_retention_tree(state, route_directory_identity(state), root.filename().string(), inventory, 4096); }
    Json inspect() const { return inspect_retention_tree(root, 4096); }
    fs::path state, root, payload;
};

void replay_partial_and_missing() {
    Fixture fixture;
    const auto plan = fixture.inspect();
    check(plan.at("root_present") == true && plan.at("payload_bytes") == 8 && plan.at("entries").size() == 5,
          "partial artifact inventory is incomplete");
    check(fixture.inspect() == plan, "read-only inventory changed the tree");
    fs::remove(fixture.payload); // Reconstruct a crash after the first persisted-inventory deletion.
    Fixture::write(fixture.root / "unknown", "do not remove");
    rejects("retention-tree-unsafe", [&] { fixture.remove(plan); });
    check(fs::exists(fixture.root / "unknown"), "unknown file was removed");
    fs::remove(fixture.root / "unknown");
    fixture.remove(plan);
    check(!fs::exists(fixture.root), "partial cleanup did not finish");
    fixture.remove(plan);
    const auto missing = fixture.inspect();
    check(missing.at("root_present") == false && missing.at("entries").empty(), "missing root was recreated");
}

void changed_and_unsafe_entries() {
    Fixture fixture;
    const auto plan = fixture.inspect();
    Fixture::write(fixture.payload, "changed\n");
    rejects("retention-tree-changed", [&] { fixture.remove(plan); });
    check(fs::exists(fixture.payload), "changed file was removed");
    rejects("retention-tree-unsafe", [&] { (void)inspect_retention_tree(fixture.root, 1); });
    const auto sentinel = fixture.state / "important-source";
    Fixture::write(sentinel, "preserve");
    fs::remove(fixture.payload);
    fs::create_symlink(sentinel, fixture.payload);
    rejects("retention-tree-unsafe", [&] { fixture.remove(plan); });
    fs::remove(fixture.payload);
    fs::create_hard_link(sentinel, fixture.payload);
    rejects("retention-tree-unsafe", [&] { (void)fixture.inspect(); });
    fs::remove(fixture.payload);
    check(::mkfifo(fixture.payload.c_str(), 0600) == 0, "fifo failed");
    rejects("retention-tree-unsafe", [&] { (void)fixture.inspect(); });
    check(fs::file_size(sentinel) == 8, "outside file changed");
    auto malicious = plan;
    malicious["entries"][0]["path"] = "../important-source";
    rejects("retention-tree-changed", [&] { fixture.remove(malicious); });
    rejects("retention-tree-changed", [&] { remove_retention_tree(fixture.state, route_directory_identity(fixture.state), "job-../../escape", plan, 4096); });
}

void replacement_root() {
    Fixture fixture;
    const auto plan = fixture.inspect();
    const auto saved = fixture.state / "saved";
    fs::rename(fixture.root, saved);
    fs::create_directory(fixture.root);
    check(::chmod(fixture.root.c_str(), 0700) == 0, "root permissions failed");
    rejects("retention-tree-changed", [&] { fixture.remove(plan); });
    check(fs::exists(saved / "docs/kb/generated/documents.jsonl") && fs::exists(fixture.root), "root replacement was deleted");
}

void writable_entries_are_rejected() {
    Fixture fixture;
    check(::chmod(fixture.payload.c_str(), 0660) == 0, "writable payload setup failed");
    rejects("retention-tree-unsafe", [&] { (void)fixture.inspect(); });
    check(::chmod(fixture.payload.c_str(), 0600) == 0, "payload restore failed");
    const auto directory = fixture.root / "docs";
    check(::chmod(directory.c_str(), 0770) == 0, "writable directory setup failed");
    rejects("retention-tree-unsafe", [&] { (void)fixture.inspect(); });
    check(::chmod(directory.c_str(), 0700) == 0, "directory restore failed");
    check(fixture.inspect().at("payload_bytes") == 8, "safe tree did not recover");
}
} // namespace

int main() {
    try {
        replay_partial_and_missing();
        changed_and_unsafe_entries();
        replacement_root();
        writable_entries_are_rejected();
        std::cout << "retention filesystem tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
