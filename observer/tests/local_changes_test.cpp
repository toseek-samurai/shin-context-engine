#include "shin/observer/local_changes.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <future>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <type_traits>
#ifdef _WIN32
#include <process.h>
#include <windows.h>
#else
#include <unistd.h>
#endif
#ifdef __APPLE__
#include <dirent.h>
#endif

namespace {
using namespace shin::observer;
using Clock = std::chrono::steady_clock;

static_assert(!std::is_copy_constructible_v<LocalChanges>);
static_assert(!std::is_move_constructible_v<LocalChanges>);

void check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

class Fixture {
public:
    Fixture() {
#ifdef _WIN32
        static std::atomic<unsigned> next{0};
        root = fs::temp_directory_path() / ("shin-local-changes-" + std::to_string(_getpid()) + "-" +
            std::to_string(Clock::now().time_since_epoch().count()) + "-" + std::to_string(next++));
        check(fs::create_directory(root), "cannot create fixture");
        root = fs::canonical(root);
#else
        auto pattern = (fs::temp_directory_path() / "shin-local-changes-XXXXXX").string();
        check(::mkdtemp(pattern.data()) != nullptr, "cannot create fixture");
        root = fs::canonical(pattern);
#endif
    }
    ~Fixture() { std::error_code error; fs::remove_all(root, error); }
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;
    fs::path root;
};

void write(const fs::path& path, const char* content) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << content;
    file.close();
    check(!file.fail(), "cannot write fixture");
}

template <typename Condition>
void await(Condition condition, const char* message) {
    const auto deadline = Clock::now() + std::chrono::seconds(5);
    while (!condition()) {
        check(Clock::now() < deadline, message);
        std::this_thread::yield();
    }
}

template <typename Action>
void sees(LocalChanges& changes, Action action) {
    changes.flush();
    const auto before = changes.sequence();
    action();
    await([&] { return changes.sequence() > before || !changes.healthy(); }, "native change was not delivered");
    changes.flush();
    check(changes.healthy(), "ordinary change lost monitor health");
    check(changes.sequence() > before, "change token did not advance");
}

void ordinary_changes_and_ignored_paths() {
    Fixture fixture;
    fs::create_directories(fixture.root / "existing" / "deep");
    fs::create_directory(fixture.root / "ignored");
    write(fixture.root / "existing" / "deep" / "file", "before");
    write(fixture.root / "ignored" / "file", "before");
    std::atomic<bool> absolute{true};
    LocalChanges changes({fixture.root}, [&](const fs::path& path, bool) {
        if (!path.is_absolute()) absolute.store(false);
        for (const auto& part : path.lexically_relative(fixture.root)) {
            if (part == "ignored" || part == "ignored-new" || part == "skip.txt") return false;
        }
        return true;
    });
    // Native delivery can run before the constructor returns, including delayed
    // fixture preparation events. A healthy invalidation token need not be zero.
    if (!changes.healthy()) {
        throw std::runtime_error("initial monitor setup is unhealthy: " + changes.failure_reason());
    }
    changes.flush();
    check(changes.healthy(), "initial native delivery lost monitor health");
    sees(changes, [&] { write(fixture.root / "existing" / "deep" / "file", "edit"); });
    sees(changes, [&] { write(fixture.root / "created", "created"); });
    sees(changes, [&] { fs::remove(fixture.root / "created"); });
    sees(changes, [&] { fs::rename(fixture.root / "existing" / "deep" / "file", fixture.root / "renamed"); });
    sees(changes, [&] { fs::remove(fixture.root / "renamed"); });
    const auto before = changes.sequence();
    write(fixture.root / "ignored" / "file", "ignored edit");
    write(fixture.root / "ignored" / "new", "ignored create");
    fs::remove(fixture.root / "ignored" / "new");
    fs::create_directories(fixture.root / "ignored-new" / "deep");
    write(fixture.root / "ignored-new" / "deep" / "new", "ignored new tree");
    write(fixture.root / "skip.txt", "ignored file");
    changes.flush();
    check(changes.healthy() && changes.sequence() == before, "ignored path invalidated monitor");
    check(absolute.load(), "predicate received a relative path");
}

void new_and_moved_directories() {
    Fixture fixture;
    Fixture outside;
    fs::create_directories(outside.root / "import" / "deep");
    write(outside.root / "import" / "deep" / "file", "before import");
    std::atomic<bool> directory_seen{false};
    LocalChanges changes({fixture.root}, [&](const fs::path&, bool directory) {
        if (directory) directory_seen.store(true);
        return true;
    });
    sees(changes, [&] {
        fs::create_directories(fixture.root / "new" / "nested");
        write(fixture.root / "new" / "nested" / "file", "created before recursive watch");
    });
    sees(changes, [&] { write(fixture.root / "new" / "nested" / "file", "after watch"); });
    sees(changes, [&] { fs::rename(outside.root / "import", fixture.root / "import"); });
    sees(changes, [&] { write(fixture.root / "import" / "deep" / "file", "after import"); });
    sees(changes, [&] { fs::rename(fixture.root / "import", fixture.root / "moved"); });
    sees(changes, [&] { write(fixture.root / "moved" / "deep" / "file", "after rename"); });
    sees(changes, [&] { fs::remove_all(fixture.root / "new"); });
    check(directory_seen.load(), "directory uncertainty was not reported");
}

void independent_roots_and_qualification() {
    Fixture first, second;
    // Select a file not created before stream startup; delayed fixture-directory
    // events cannot be mistaken for an edit in the independently monitored root.
    auto relevant = [](const fs::path& path, bool) { return path.filename() == "file"; };
    LocalChanges one({first.root}, relevant), two({second.root}, relevant);
    LocalChanges both({first.root, second.root}, relevant);
    one.flush(); two.flush(); both.flush();
    const auto second_before = two.sequence();
    const auto both_before = both.sequence();
    sees(one, [&] { write(first.root / "file", "first root"); });
    await([&] {
        both.flush();
        return both.sequence() > both_before;
    }, "multi-root monitor did not receive first root event");
    two.flush();
    check(two.sequence() == second_before, "edit in first root reached independent second monitor");
    sees(two, [&] { write(second.root / "file", "second root"); });
    both.flush();
    check(both.healthy(), "multi-root monitor lost health");
    const auto before_qualification = both.sequence();
    write(first.root / "file", "during source qualification");
    await([&] {
        both.flush();
        return both.sequence() > before_qualification;
    }, "qualification event was not delivered within its bound");
    check(both.sequence() > before_qualification, "qualification lost an intervening event");
}

void failure_is_explicit_and_sticky() {
    Fixture fixture;
    auto relevant = [](const fs::path&, bool) { return true; };
    LocalChanges missing({fixture.root / "absent"}, relevant);
    check(!missing.healthy() && missing.sequence() > 0, "missing root claimed healthy monitoring");
    missing.flush();
    check(!missing.healthy(), "failed monitor recovered implicitly");
    fs::create_directory_symlink(fixture.root, fixture.root / "link");
    LocalChanges symlink({fixture.root / "link"}, relevant);
    check(!symlink.healthy() && symlink.sequence() > 0, "symlink root was traversed");
    LocalChanges lost({fixture.root}, [](const fs::path&, bool) { return false; });
    const auto before = lost.sequence();
    // Root loss is not suppressible by filtering.
    const auto destination = fixture.root.string() + "-moved";
    fs::rename(fixture.root, destination);
    fixture.root = destination;
    lost.flush();
    await([&] { return !lost.healthy(); }, "root rename did not invalidate health");
    check(lost.sequence() > before, "root loss did not advance token");
    lost.flush();
    check(!lost.healthy(), "root loss health was not sticky");
}

void root_deletion_and_ancestor_loss() {
    Fixture fixture;
    const auto root = fixture.root / "parent" / "root";
    fs::create_directories(root);
    {
    LocalChanges changes({root}, [](const fs::path&, bool) { return false; });
    changes.flush();
    const auto before = changes.sequence();
#ifdef _WIN32
    // NTFS refuses an ordinary ancestor rename with an open descendant,
    // even with FILE_SHARE_DELETE. This remains a Windows release limitation.
    std::error_code error;
    fs::rename(fixture.root / "parent", fixture.root / "renamed-parent", error);
    check(error.value() == ERROR_ACCESS_DENIED, "unexpected native ancestor rename outcome");
    changes.flush();
    check(changes.healthy() && changes.sequence() == before, "refused rename damaged the unchanged root");
#else
    fs::rename(fixture.root / "parent", fixture.root / "renamed-parent");
    changes.flush();
    await([&] { return !changes.healthy(); }, "ancestor rename did not invalidate health");
    check(changes.sequence() > before, "ancestor loss did not advance token");
#endif
    }
#ifdef _WIN32
    {
        // Reproduce the restriction without Shin monitoring or pinned IO.
        const auto raw = CreateFileW(root.c_str(), FILE_LIST_DIRECTORY,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
            OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        check(raw != INVALID_HANDLE_VALUE, "cannot open native rename control");
        std::error_code error;
        fs::rename(fixture.root / "parent", fixture.root / "renamed-parent", error);
        CloseHandle(raw);
        check(error.value() == ERROR_ACCESS_DENIED, "ancestor restriction was not reproduced without Shin");
    }
    fs::rename(fixture.root / "parent", fixture.root / "renamed-parent");
#endif

    fs::create_directory(fixture.root / "delete-root");
    LocalChanges deleted({fixture.root / "delete-root"}, [](const fs::path&, bool) { return false; });
    fs::remove(fixture.root / "delete-root");
    deleted.flush();
    await([&] { return !deleted.healthy(); }, "root deletion did not invalidate health");
    check(deleted.sequence() > 0, "root deletion did not advance token");
}

void predicate_failure_and_teardown() {
    Fixture fixture;
    write(fixture.root / "file", "before");
    std::atomic<bool> throwing{false};
    {
        LocalChanges changes({fixture.root}, [&](const fs::path&, bool) {
            if (throwing.load()) throw std::runtime_error("predicate failure");
            return true;
        });
        changes.flush();
        throwing.store(true);
        write(fixture.root / "file", "after");
        changes.flush();
        await([&] { return !changes.healthy(); }, "callback exception was not contained");
        check(changes.sequence() > 0, "predicate failure did not invalidate token");
    }
    std::atomic<int> callbacks{0};
    for (int i = 0; i < 20; ++i) {
        LocalChanges changes({fixture.root}, [&](const fs::path&, bool) { ++callbacks; return true; });
        write(fixture.root / "file", "teardown with queued events");
        if (i % 2 == 0) changes.flush();
    }
    const auto after_destruction = callbacks.load();
    // Delivery through another native stream is a fence, not a fixed sleep.
    LocalChanges fence({fixture.root}, [](const fs::path&, bool) { return true; });
    sees(fence, [&] { write(fixture.root / "file", "after destruction"); });
    check(callbacks.load() == after_destruction, "callback outlived its monitor");
}

#ifdef __APPLE__
unsigned descriptor_count() {
    auto* directory = ::opendir("/dev/fd");
    check(directory != nullptr, "cannot count native descriptors");
    unsigned count = 0;
    while (const auto* entry = ::readdir(directory))
        if (entry->d_name[0] >= '0' && entry->d_name[0] <= '9') ++count;
    ::closedir(directory);
    return count;
}

void shared_roots_preserve_scope_lifetime_and_root_loss() {
    Fixture fixture;
    const auto root = fixture.root;
    write(root / "first", "before");
    // The second path is untouched until its own action. Delayed FSEvents
    // setup notifications must not be mistaken for cross-scope delivery.
    auto first = std::make_unique<LocalChanges>(std::vector<fs::path>{root},
        [path = root / "first"](const fs::path& p, bool) { return p == path; });
    first->flush();
    const auto one = descriptor_count();
    auto second = std::make_unique<LocalChanges>(std::vector<fs::path>{root},
        [path = root / "second"](const fs::path& p, bool) { return p == path; });
    std::vector<std::unique_ptr<LocalChanges>> peers;
    for (unsigned i = 0; i < 6; ++i)
        peers.push_back(std::make_unique<LocalChanges>(std::vector<fs::path>{root},
            [](const fs::path&, bool) { return false; }));
    second->flush();
    check(descriptor_count() <= one + 4, "identical roots retained duplicate native descriptors");
    const auto before_second = second->sequence();
    sees(*first, [&] { write(root / "first", "edited first"); });
    second->flush();
    check(second->healthy() && second->sequence() == before_second, "one scope invalidated its peer");
    first.reset();
    sees(*second, [&] { write(root / "second", "edited second"); });
    const auto before_loss = second->sequence();
    const auto moved = root.string() + "-moved";
    fs::rename(root, moved); fixture.root = moved;
    second->flush();
    await([&] { return !second->healthy(); }, "shared root loss was missed");
    check(second->sequence() > before_loss, "shared root loss did not invalidate the token");
    for (const auto& peer : peers) check(!peer->healthy(), "filter suppressed shared root loss");
    Fixture replacement;
    fs::remove(replacement.root); replacement.root = root; fs::create_directory(root);
    LocalChanges recovered({root}, [](const fs::path&, bool) { return true; });
    sees(recovered, [&] { write(root / "new", "replacement root"); });
    check(!second->healthy(), "replacement stream revived an invalidated subscription");
}

void concurrent_shared_root_subscriptions() {
    Fixture fixture;
    std::vector<std::future<std::unique_ptr<LocalChanges>>> pending;
    for (unsigned i = 0; i < 8; ++i)
        pending.push_back(std::async(std::launch::async, [root = fixture.root] {
            auto monitor = std::make_unique<LocalChanges>(std::vector<fs::path>{root},
                [](const fs::path&, bool) { return true; });
            monitor->flush(); return monitor;
        }));
    std::vector<std::unique_ptr<LocalChanges>> monitors;
    std::vector<std::uint64_t> before;
    for (auto& future : pending) monitors.push_back(future.get());
    for (auto& monitor : monitors) monitor->flush();
    for (auto& monitor : monitors) before.push_back(monitor->sequence());
    write(fixture.root / "created", "shared delivery");
    for (std::size_t i = 0; i < monitors.size(); ++i) {
        await([&] { return monitors[i]->sequence() > before[i]; }, "concurrent subscriber missed delivery");
        check(monitors[i]->healthy(), "concurrent subscription lost native health");
    }
}

void shared_callback_does_not_pin_peer_teardown() {
    Fixture fixture;
    std::atomic<bool> armed{false};
    std::promise<void> entered, release;
    auto released = release.get_future().share();
    LocalChanges first({fixture.root}, [&](const fs::path&, bool) {
        if (armed.exchange(false)) { entered.set_value(); released.wait(); }
        return true;
    });
    auto peer = std::make_unique<LocalChanges>(std::vector<fs::path>{fixture.root},
        [](const fs::path&, bool) { return false; });
    struct Release {
        std::promise<void>& trigger; bool done = false;
        ~Release() { if (!done) trigger.set_value(); }
    } guard{release};
    first.flush(); peer->flush(); armed.store(true);
    write(fixture.root / "created", "hold one subscriber");
    check(entered.get_future().wait_for(std::chrono::seconds(5)) == std::future_status::ready,
          "shared predicate was not entered");
    auto closing = std::async(std::launch::async, [&] { peer.reset(); });
    const auto status = closing.wait_for(std::chrono::seconds(1));
    release.set_value(); guard.done = true;
    closing.get();
    check(status == std::future_status::ready, "unrelated predicate blocked peer teardown");
    first.flush(); check(first.healthy(), "peer teardown damaged its live subscription");
}
#endif

#if defined(__linux__) || defined(_WIN32)
void no_symlink_subtree_traversal() {
    Fixture fixture, outside;
    write(outside.root / "file", "before");
    fs::create_directory_symlink(outside.root, fixture.root / "link");
    LocalChanges changes({fixture.root}, [](const fs::path&, bool) { return true; });
    check(changes.healthy(), "symlink entry made setup unhealthy");
    changes.flush();
    const auto before = changes.sequence();
    write(outside.root / "file", "outside edit");
    changes.flush();
    check(changes.sequence() == before, "monitor followed a source symlink");
}
#endif

#ifdef _WIN32
void native_bounds_and_delivery() {
    Fixture fixture;
    auto root = fixture.root / L"\u043f\u0440\u043e\u0435\u043a\u0442";
    for (int i = 0; i < 4; ++i) root /= std::string(70, 'a') + std::to_string(i);
    fs::create_directories(root);
    {
        LocalChanges changes({root}, [](const fs::path&, bool) { return true; });
        check(changes.healthy(), "Unicode/long root is not monitored");
        sees(changes, [&] { write(root / L"\u043a\u043e\u0434.cpp", "long path edit"); });
    }
    DWORD before = 0, after = 0;
    check(GetProcessHandleCount(GetCurrentProcess(), &before), "cannot count native handles");
    for (int i = 0; i < 30; ++i) {
        LocalChanges changes({fixture.root}, [](const fs::path&, bool) { return true; });
        check(changes.healthy(), "repeated setup failed");
        sees(changes, [&] { write(fixture.root / "handle-test", "queued delivery"); });
    }
    check(GetProcessHandleCount(GetCurrentProcess(), &after) && after <= before,
        "native change handles survived teardown");

    {
        std::atomic<LocalChanges*> current{nullptr};
        LocalChanges changes({fixture.root}, [&](const fs::path& path, bool) {
            if (path.filename() == "reenter") current.load()->flush();
            return true;
        });
        current.store(&changes);
        write(fixture.root / "reenter", "no callback deadlock");
        changes.flush();
        await([&] { return !changes.healthy(); }, "callback reentry did not fail closed");
    }
    auto too_deep = fixture.root;
    for (int i = 0; i < 64; ++i) too_deep /= "d";
    fs::create_directories(too_deep);
    {
        LocalChanges limited({too_deep}, [](const fs::path&, bool) { return true; });
        check(!limited.healthy() && limited.sequence() > 0, "native handle budget was not enforced");
    }

    std::promise<void> entered, release;
    auto started = entered.get_future();
    const auto released = release.get_future().share();
    std::atomic<bool> once{false};
    LocalChanges overflow({fixture.root}, [&](const fs::path& path, bool) {
        if (path.filename() == "hold" && !once.exchange(true)) {
            entered.set_value();
            released.wait();
        }
        return true;
    });
    // Always release delivery before destroying the monitor, including failures.
    struct Release {
        std::promise<void>& promise;
        bool done = false;
        ~Release() { if (!done) promise.set_value(); }
    } guard{release};
    write(fixture.root / "hold", "block callback");
    check(started.wait_for(std::chrono::seconds(5)) == std::future_status::ready, "native delivery did not start");
    for (int i = 0; i < 600; ++i) write(fixture.root / (std::string(80, 'f') + std::to_string(i)), "overflow");
    release.set_value(); guard.done = true;
    await([&] { return !overflow.healthy(); }, "native overflow did not invalidate health");
    overflow.flush();
    check(!overflow.healthy() && overflow.sequence() > 0, "overflow failure was not sticky");
}
#endif
} // namespace

int main() {
    try {
#if defined(__APPLE__) || defined(__linux__) || defined(_WIN32)
        ordinary_changes_and_ignored_paths();
        new_and_moved_directories();
        independent_roots_and_qualification();
        failure_is_explicit_and_sticky();
        root_deletion_and_ancestor_loss();
        predicate_failure_and_teardown();
#ifdef __APPLE__
        shared_roots_preserve_scope_lifetime_and_root_loss();
        concurrent_shared_root_subscriptions();
        shared_callback_does_not_pin_peer_teardown();
#endif
#if defined(__linux__) || defined(_WIN32)
        no_symlink_subtree_traversal();
#endif
#ifdef _WIN32
        native_bounds_and_delivery();
#endif
#else
        Fixture fixture;
        LocalChanges unsupported({fixture.root}, [](const fs::path&, bool) { return true; });
        check(!unsupported.healthy() && unsupported.sequence() > 0, "unsupported monitor claimed health");
#endif
        std::cout << "bounded native local changes passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
