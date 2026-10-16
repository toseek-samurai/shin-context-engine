#include "shin/observer/io.h"
#include "shin/observer/observer.h"

#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
using namespace shin::observer;
namespace fs = std::filesystem;

void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template <typename Action>
void rejects(const char* code, Action action) {
    try {
        action();
    } catch (const Error& error) {
        check(error.code == code, "unexpected observer error code");
        return;
    }
    throw std::runtime_error("unsafe operation succeeded");
}

void write_private(const fs::path& path, const std::string& content) {
    const int descriptor = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    check(descriptor >= 0, "fixture file open failed");
    std::size_t offset = 0;
    while (offset < content.size()) {
        const ssize_t count = ::write(descriptor, content.data() + offset, content.size() - offset);
        check(count > 0, "fixture file write failed");
        offset += static_cast<std::size_t>(count);
    }
    check(::close(descriptor) == 0, "fixture file close failed");
    check(::chmod(path.c_str(), 0600) == 0, "fixture file chmod failed");
}

class Fixture {
public:
    Fixture() {
        std::string pattern = (fs::temp_directory_path() / "shin-observer-io-XXXXXX").string();
        check(::mkdtemp(pattern.data()) != nullptr, "fixture root failed");
        root = fs::canonical(pattern);
        state = root / "state";
        check(::mkdir(state.c_str(), 0700) == 0, "fixture state directory failed");
        check(::chmod(state.c_str(), 0700) == 0, "fixture state chmod failed");
    }
    ~Fixture() { std::error_code error; fs::remove_all(root, error); }
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;

    fs::path root;
    fs::path state;
};

void reader_is_non_mutating() {
    Fixture fixture;
    StateStore reader(fixture.state, false);
    check(!reader.load().has_value(), "empty state should be absent");
    check(!reader.writer_present(), "missing lock should not be reported as held");
    check(!fs::exists(fixture.state / "observer.lock") && !fs::exists(fixture.state / "state.json") &&
              !fs::exists(fixture.state / "state.pending"),
          "reader created state files");

    write_private(fixture.state / "state.pending", "{\"discard\":true}");
    StateStore pending_reader(fixture.state, false);
    check(!pending_reader.recovered_pending() && fs::exists(fixture.state / "state.pending"),
          "reader recovered pending state");
}

void private_directory_exclusive_creation() {
    Fixture fixture;
    const auto path = fixture.root / "candidate";
    ensure_private_directory(path, true);
    const auto identity = directory_identity(path, true);
    rejects("directory-create-failed", [&] { ensure_private_directory(path, true); });
    ensure_private_directory(path);
    check(directory_identity(path, true) == identity, "exclusive refusal changed directory identity");
    fs::create_directory_symlink(path, fixture.root / "alias");
    rejects("directory-create-failed", [&] { ensure_private_directory(fixture.root / "alias", true); });
}

void writer_exclusion_and_atomic_state() {
    Fixture fixture;
    int start[2] {};
    check(::pipe(start) == 0, "writer exclusion pipe failed");
    const pid_t child = ::fork();
    check(child >= 0, "fork failed");
    if (child == 0) {
        ::close(start[1]);
        char signal = 0;
        if (::read(start[0], &signal, 1) != 1) _exit(3);
        ::close(start[0]);
        try {
            StateStore competing(fixture.state, true);
        } catch (const Error& error) {
            _exit(error.code == "writer-busy" ? 0 : 2);
        }
        _exit(1);
    }
    ::close(start[0]);
    StateStore writer(fixture.state, true);
    check(writer.writer_present(), "writer lock is not reported as held");
    check(::write(start[1], "x", 1) == 1, "writer exclusion signal failed");
    check(::close(start[1]) == 0, "writer exclusion pipe close failed");
    int status = 0;
    check(::waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "competing writer acquired lock");

    writer.save(Json{{"generation", 1}, {"values", Json::array({"one", "two"})}});
    StateStore reader(fixture.state, false);
    const auto first = reader.load();
    check(first.has_value() && first->at("generation") == 1, "first atomic state was not readable");
    writer.save(Json{{"generation", 2}});
    const auto second = reader.load();
    check(second.has_value() && second->at("generation") == 2, "replacement state was not readable");
    check(!fs::exists(fixture.state / "state.pending"), "committed pending state remained visible");
}

void pending_recovery_and_state_bounds() {
    Fixture fixture;
    write_private(fixture.state / "state.pending", "{\"interrupted\":true}");
    StateStore writer(fixture.state, true);
    check(!writer.recovered_pending() && fs::exists(fixture.state / "state.pending"), "constructor recovered unverified state");
    writer.recover_pending();
    check(writer.recovered_pending() && !fs::exists(fixture.state / "state.pending"), "safe pending state was not recovered");
    rejects("state-too-large", [&] { writer.save(Json{{"bytes", std::string(256 * 1024, 'x')}}); });

    write_private(fixture.state / "state.json", std::string(256 * 1024 + 1, 'x'));
    StateStore reader(fixture.state, false);
    rejects("state-too-large", [&] { (void)reader.load(); });

    write_private(fixture.state / "state.json", "{\"broken\":");
    rejects("invalid-json", [&] { (void)reader.load(); });
}

void json_and_file_safety() {
    check(parse_json("{\"nested\":[1,2,3]}").at("nested").size() == 3, "valid JSON rejected");
    rejects("invalid-json", [] { (void)parse_json("{\"x\":1,\"x\":2}"); });
    rejects("invalid-json", [] { (void)parse_json("{\"a\":{\"x\":1,\"x\":2}}"); });
    rejects("invalid-json", [] { (void)parse_json(std::string(33, '[') + "0" + std::string(33, ']')); });

    Fixture fixture;
    const fs::path target = fixture.root / "target";
    write_private(target, "safe");
    check(read_text(target, 16, true) == "safe", "safe file was unreadable");
    rejects("file-too-large", [&] { (void)read_text(target, 3, true); });
    fs::create_symlink(target, fixture.root / "link");
    rejects("unsafe-file", [&] { (void)read_text(fixture.root / "link", 16, true); });
    fs::create_hard_link(target, fixture.root / "hard-link");
    rejects("unsafe-file", [&] { (void)read_text(target, 16, true); });

    const fs::path safe_directory = fixture.root / "safe-directory";
    check(::mkdir(safe_directory.c_str(), 0700) == 0, "safe directory creation failed");
    check(::chmod(safe_directory.c_str(), 0700) == 0, "safe directory chmod failed");
    write_private(safe_directory / "file", "content");
    fs::create_directory_symlink(safe_directory, fixture.root / "directory-link");
    rejects("unsafe-file", [&] { (void)read_text(fixture.root / "directory-link" / "file", 16, true); });

    Fixture state_fixture;
    const fs::path state_target = state_fixture.root / "state-target";
    write_private(state_target, "{\"value\":1}");
    fs::create_symlink(state_target, state_fixture.state / "state.json");
    StateStore state_reader(state_fixture.state, false);
    rejects("state-unsafe", [&] { (void)state_reader.load(); });
    fs::remove(state_fixture.state / "state.json");
    fs::create_hard_link(state_target, state_fixture.state / "state.json");
    rejects("state-unsafe", [&] { (void)state_reader.load(); });
}

void directory_replacement_is_rejected() {
    Fixture fixture;
    StateStore writer(fixture.state, true);
    const fs::path old_state = fixture.root / "old-state";
    fs::rename(fixture.state, old_state);
    check(::mkdir(fixture.state.c_str(), 0700) == 0, "replacement state directory creation failed");
    check(::chmod(fixture.state.c_str(), 0700) == 0, "replacement state directory chmod failed");
    rejects("directory-changed", [&] { writer.save(Json{{"generation", 1}}); });
    const auto old_identity = directory_identity(old_state, true);
    rejects("directory-changed", [&] { StateStore wrong_binding(fixture.state, true, old_identity); });
    check(!fs::exists(fixture.state / "observer.lock"), "mismatched directory identity created a writer lock");
}

void archive_exclusive_create_and_write_failure() {
    Fixture fixture;
    const auto archive = fixture.root / "archive.json";
    check(write_private_archive(archive, "immutable"), "archive was not created");
    check(!write_private_archive(archive, "immutable"), "exact archive retry rewrote the file");
    rejects("archive-conflict", [&] { (void)write_private_archive(archive, "replacement"); });
    check(read_text(archive, 256, true) == "immutable", "different archive retry replaced bytes");
    fs::create_hard_link(archive, fixture.root / "hard-link");
    rejects("unsafe-file", [&] { (void)write_private_archive(archive, "immutable"); });
    rejects("archive-too-large", [&] { (void)write_private_archive(fixture.root / "large", std::string(kMaximumStateBytes + 1, 'x')); });
    check(!fs::exists(fixture.root / "large"), "oversized archive left a file");
    const auto incomplete = fixture.root / "incomplete";
    const pid_t child = ::fork();
    check(child >= 0, "archive failure fork failed");
    if (child == 0) {
        const rlimit limit{64, 64};
        if (::setrlimit(RLIMIT_FSIZE, &limit) != 0 || std::signal(SIGXFSZ, SIG_IGN) == SIG_ERR) _exit(2);
        try { (void)write_private_archive(incomplete, std::string(1024, 'x')); }
        catch (const Error& error) { _exit(error.code == "state-write-failed" ? 0 : 3); }
        _exit(1);
    }
    int status = 0;
    check(::waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "bounded archive write failure was not reported");
    const auto partial = read_text(incomplete, 1024, true);
    check(partial.size() <= 64, "failed archive exceeded the injected filesystem bound");
    rejects("archive-conflict", [&] { (void)write_private_archive(incomplete, std::string(1024, 'x')); });
    check(read_text(incomplete, 1024, true) == partial, "retry overwrote an incomplete archive");
    rejects("writer-unavailable", [&] { StateStore existing_only(fixture.state, true, {}, false); });
    check(!fs::exists(fixture.state / "observer.lock"), "existing-only fence created a lock");
}

void user_archive_ownership_and_removal() {
    Fixture fixture;
    check(::chmod(fixture.state.c_str(), 0755) == 0, "public parent chmod failed");
    const auto agent = fixture.state / "agent.plist";
    rejects("unsafe-directory", [&] { (void)write_private_archive(agent, "owned"); });
    check(write_user_archive(agent, "owned"), "user archive was not created");
    check(!write_user_archive(agent, "owned"), "user archive retry rewrote it");
    rejects("archive-conflict", [&] { remove_user_archive(agent, "different"); });
    check(read_text(agent, 32, true) == "owned", "changed delete removed archive");
    fs::create_hard_link(agent, fixture.state / "linked");
    rejects("unsafe-file", [&] { remove_user_archive(agent, "owned"); });
    fs::remove(fixture.state / "linked");
    check(::chmod(fixture.state.c_str(), 0777) == 0, "unsafe parent chmod failed");
    rejects("unsafe-directory", [&] { remove_user_archive(agent, "owned"); });
    rejects("unsafe-directory", [&] { (void)write_user_archive(fixture.state / "other", "data"); });
    check(::chmod(fixture.state.c_str(), 0755) == 0, "parent restore failed");
    remove_user_archive(agent, "owned");
    check(!fs::exists(agent), "owned archive was not removed");
}

void user_archive_atomic_replacement() {
    Fixture fixture;
    check(::chmod(fixture.state.c_str(), 0755) == 0, "parent chmod failed");
    const auto agent = fixture.state / "agent.plist";
    const auto identity = directory_identity(fixture.state);
    (void)write_user_archive(agent, "original");
    rejects("unsafe-directory", [&] { replace_user_archive(agent, "original", "next", "wrong"); });
    rejects("archive-conflict", [&] { replace_user_archive(agent, "foreign", "next", identity); });
    fs::create_hard_link(agent, fixture.state / "linked");
    rejects("unsafe-file", [&] { replace_user_archive(agent, "original", "next", identity); });
    fs::remove(fixture.state / "linked");
    const auto child = ::fork();
    check(child >= 0, "replacement failure fork failed");
    if (child == 0) {
        const rlimit limit{64, 64};
        if (::setrlimit(RLIMIT_FSIZE, &limit) != 0 || std::signal(SIGXFSZ, SIG_IGN) == SIG_ERR) _exit(2);
        try { replace_user_archive(agent, "original", std::string(1024, 'x'), identity); }
        catch (const Error& error) { _exit(error.code == "state-write-failed" ? 0 : 3); }
        _exit(1);
    }
    int status = 0;
    check(::waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "replacement write failure was not reported");
    check(read_text(agent, 32, true) == "original", "failed staging truncated the live file");
    check(std::distance(fs::directory_iterator(fixture.state), fs::directory_iterator{}) == 1,
          "failed staging leaked its temporary file");
    replace_user_archive(agent, "original", "next", identity);
    check(read_text(agent, 32, true) == "next", "replacement was not published");
    replace_user_archive(agent, "original", "next", identity);
    check(read_text(agent, 32, true) == "next", "interrupted publication retry failed");
}

} // namespace

int main(int argc, char** argv) {
    try {
        // Read-only native diagnostics for explicitly reviewed local recovery.
        if (argc == 3 && std::string_view(argv[1]) == "--identity") {
            std::cout << Json{{"path", argv[2]}, {"identity", directory_identity(argv[2])}}.dump() << '\n';
            return 0;
        }
        if (argc == 3 && std::string_view(argv[1]) == "--config-binding") {
            const auto config = load_config(argv[2]);
            std::cout << Json{{"binding", config.binding}, {"binding_key", config.binding_key}}.dump() << '\n';
            return 0;
        }
        if (argc != 1) return 2;
        reader_is_non_mutating();
        private_directory_exclusive_creation();
        writer_exclusion_and_atomic_state();
        pending_recovery_and_state_bounds();
        json_and_file_safety();
        directory_replacement_is_rejected();
        archive_exclusive_create_and_write_failure();
        user_archive_ownership_and_removal();
        user_archive_atomic_replacement();
        std::cout << "observer io tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
