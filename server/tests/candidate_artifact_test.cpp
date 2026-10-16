#include "shin/server/candidate_artifact.h"
#include "shin/server/route_files.h"

#include <csignal>
#include <iostream>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
using namespace shin::server;
namespace fs = std::filesystem;
using Mode = CandidateArtifactLock::Mode;
const std::string key(64, 'a');

void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template <typename Function>
void rejects(const char* code, Function action) {
    try { action(); }
    catch (const ServiceError& error) {
        check(error.code == code, "unexpected candidate lock error");
        return;
    }
    throw std::runtime_error("unsafe candidate operation succeeded");
}

class Fixture {
public:
    Fixture() {
        auto name = (fs::temp_directory_path() / "shin-candidate-XXXXXX").string();
        check(::mkdtemp(name.data()) != nullptr, "temporary directory failed");
        root = fs::canonical(name);
        identity = route_directory_identity(root);
    }
    ~Fixture() { std::error_code error; fs::remove_all(root, error); }
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;
    fs::path root;
    std::string identity;
};

void existing_only_and_ownership(Fixture& fixture) {
    const auto name = fixture.root / (".job-1-" + key + ".lock");
    rejects("candidate-lock-missing", [&] {
        const CandidateArtifactLock missing(fixture.root, fixture.identity, 1, key, Mode::inspect_existing);
    });
    check(!fs::exists(name), "read-only inspection created a lock");
    {
        const CandidateArtifactLock writer(fixture.root, fixture.identity, 1, key, Mode::create_writer);
        rejects("candidate-artifact-busy", [&] {
            const CandidateArtifactLock reader(fixture.root, fixture.identity, 1, key, Mode::inspect_existing);
        });
        rejects("candidate-lock-exists", [&] {
            const CandidateArtifactLock duplicate(fixture.root, fixture.identity, 1, key, Mode::create_writer);
        });
    }
    {
        const CandidateArtifactLock first(fixture.root, fixture.identity, 1, key, Mode::inspect_existing);
        const CandidateArtifactLock second(fixture.root, fixture.identity, 1, key, Mode::inspect_existing);
        first.verify();
        second.verify();
        rejects("candidate-artifact-busy", [&] {
            const CandidateArtifactLock cleanup(fixture.root, fixture.identity, 1, key, Mode::cleanup_existing);
        });
    }
    {
        const CandidateArtifactLock cleanup(fixture.root, fixture.identity, 1, key, Mode::cleanup_existing);
        rejects("candidate-artifact-busy", [&] {
            const CandidateArtifactLock reader(fixture.root, fixture.identity, 1, key, Mode::inspect_existing);
        });
    }
    check(fs::exists(name) && fs::file_size(name) == 0, "lock identity was removed on close");
    fs::create_hard_link(name, fixture.root / "alias");
    rejects("candidate-lock-unsafe", [&] {
        const CandidateArtifactLock aliased(fixture.root, fixture.identity, 1, key, Mode::inspect_existing);
    });
    fs::remove(fixture.root / "alias");
    fs::create_symlink(name, fixture.root / (".job-2-" + key + ".lock"));
    rejects("candidate-lock-unsafe", [&] {
        const CandidateArtifactLock symlink(fixture.root, fixture.identity, 2, key, Mode::inspect_existing);
    });
    check(::mkfifo((fixture.root / (".job-3-" + key + ".lock")).c_str(), 0600) == 0, "fifo fixture failed");
    rejects("candidate-lock-unsafe", [&] {
        const CandidateArtifactLock fifo(fixture.root, fixture.identity, 3, key, Mode::inspect_existing);
    });
    {
        const CandidateArtifactLock held(fixture.root, fixture.identity, 1, key, Mode::inspect_existing);
        fs::rename(name, fixture.root / "old-lock");
        { const CandidateArtifactLock replacement(fixture.root, fixture.identity, 1, key, Mode::create_writer); }
        rejects("candidate-lock-unsafe", [&] { held.verify(); });
    }
    check(::chmod(name.c_str(), 0644) == 0, "chmod fixture failed");
    rejects("candidate-lock-unsafe", [&] {
        const CandidateArtifactLock exposed(fixture.root, fixture.identity, 1, key, Mode::inspect_existing);
    });
}

class Child {
public:
    explicit Child(pid_t pid) : pid_(pid) {}
    ~Child() { terminate(); }
    Child(const Child&) = delete;
    Child& operator=(const Child&) = delete;
    void terminate() {
        if (pid_ <= 0) return;
        ::kill(pid_, SIGKILL);
        int status = 0;
        while (::waitpid(pid_, &status, 0) < 0 && errno == EINTR) {}
        pid_ = -1;
    }
private:
    pid_t pid_;
};

void process_lifetime(Fixture& fixture) {
    int signal_pipe[2]{};
    check(::pipe(signal_pipe) == 0, "pipe failed");
    const auto pid = ::fork();
    if (pid < 0) {
        ::close(signal_pipe[0]);
        ::close(signal_pipe[1]);
        throw std::runtime_error("fork failed");
    }
    if (pid == 0) {
        ::close(signal_pipe[0]);
        try {
            const CandidateArtifactLock writer(fixture.root, fixture.identity, 20, key, Mode::create_writer);
            const char ready = '1';
            if (::write(signal_pipe[1], &ready, 1) != 1) ::_exit(2);
            ::close(signal_pipe[1]);
            while (true) ::pause();
        } catch (...) { ::_exit(3); }
    }
    Child child(pid);
    ::close(signal_pipe[1]);
    char ready = 0;
    const auto received = ::read(signal_pipe[0], &ready, 1);
    ::close(signal_pipe[0]);
    check(received == 1 && ready == '1', "child did not acquire its lock");
    // No elapsed DB lease can release a paused process's descriptor.
    rejects("candidate-artifact-busy", [&] {
        const CandidateArtifactLock inspection(fixture.root, fixture.identity, 20, key, Mode::inspect_existing);
    });
    child.terminate();
    const CandidateArtifactLock recovered(fixture.root, fixture.identity, 20, key, Mode::inspect_existing);
    recovered.verify();
}
} // namespace

int main() {
    try {
        Fixture fixture;
        existing_only_and_ownership(fixture);
        process_lifetime(fixture);
        std::cout << "candidate artifact lifecycle tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
