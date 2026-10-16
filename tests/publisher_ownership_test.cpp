#include "generated_io.h"
#include "context_internal.h"
#include <chrono>
#include <future>
#include <iostream>
#ifdef _WIN32
#include "windows_private_acl.h"
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {
namespace io = shin::generated_io;
namespace fs = std::filesystem;
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class Action> void refused(Action action) {
    try { action(); } catch (const std::runtime_error&) { return; }
    throw std::runtime_error("Expected publisher ownership refusal");
}
void run() {
#ifdef _WIN32
    const auto base = fs::temp_directory_path() / ("shin-publisher-" + std::to_string(GetCurrentProcessId()) + "-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    check(fs::create_directory(base), "Fixture creation failed");
    fixture_private_acl(base);
#else
    std::string pattern = "/tmp/shin-publisher-XXXXXX";
    check(::mkdtemp(pattern.data()) != nullptr, "Fixture creation failed");
    const auto base = fs::canonical(pattern);
#endif
    struct Cleanup { fs::path root; ~Cleanup() { std::error_code error; fs::remove_all(root, error); } } cleanup{base};
    const auto out = base / "index";
    const auto claim = out / "docs/kb/.shin-context-owner";
    const std::string a(64, 'a'), b(64, 'b');
    check(!io::publisher_owner(out), "Read-only lookup invented an owner");
    check(!fs::exists(out), "Read-only lookup created storage");
    {
        io::IndexLock writer(out, io::LockMode::exclusive, io::LockTarget::writer);
        io::publish_snapshot(out, "docs/kb/generated", {{"docs/kb/generated/manifest.json", "original"}});
    }
    refused([&] { io::claim_publisher(out, "invalid"); });
    io::claim_publisher(out, a);
    io::claim_publisher(out, a);
    check(io::publisher_owner(out) == a, "Owner was not retained");
    refused([&] { io::claim_publisher(out, b); });
    refused([&] { io::release_publisher(out, b); });
    refused([&] { io::IndexLock writer(out, io::LockMode::exclusive, io::LockTarget::writer); });
    refused([&] { io::IndexLock writer(out, io::LockMode::exclusive, io::LockTarget::writer, b); });
    shin::context::RuntimeOptions options;
    options.out_root = out;
    refused([&] { shin::context::run_build(options); });
    refused([&] { io::prune_snapshot(out, "docs/kb/generated", "original"); });
    check(shin::context::read_file(out / "docs/kb/generated/manifest.json") == "original",
          "Rejected writer modified the active snapshot");
    {
        io::IndexLock read(out, io::LockMode::shared_existing);
        check(io::publisher_owner(out) == a, "Readers were blocked by ownership");
    }
    auto writer = std::make_unique<io::IndexLock>(out, io::LockMode::exclusive, io::LockTarget::writer, a);
    io::publish_snapshot(out, "docs/kb/generated", {{"docs/kb/generated/manifest.json", "new"}});
    std::promise<void> started;
    auto revoke = std::async(std::launch::async, [&] { started.set_value(); io::release_publisher(out, a); });
    started.get_future().wait();
    check(revoke.wait_for(std::chrono::milliseconds(50)) == std::future_status::timeout,
          "Revocation escaped an active publication fence");
    writer.reset();
    revoke.get();
    io::release_publisher(out, a);
    refused([&] { io::IndexLock stale_candidate(out, io::LockMode::exclusive, io::LockTarget::writer, a); });
    { io::IndexLock manual(out, io::LockMode::exclusive, io::LockTarget::writer); }
    io::claim_publisher(out, b);
    refused([&] { io::IndexLock stale_candidate(out, io::LockMode::exclusive, io::LockTarget::writer, a); });
    fs::create_hard_link(claim, base / "linked");
    refused([&] { io::publisher_owner(out); });
    refused([&] { io::release_publisher(out, b); });
    fs::remove(base / "linked");
    io::release_publisher(out, b);
    fs::create_symlink(base / "missing", claim);
    refused([&] { io::publisher_owner(out); });
    refused([&] { io::IndexLock manual(out, io::LockMode::exclusive, io::LockTarget::writer); });
    fs::remove(claim);
    { std::ofstream file(claim); file << "partial"; }
#ifdef _WIN32
    fixture_private_acl(claim);
#else
    ::chmod(claim.c_str(), 0600);
#endif
    refused([&] { io::claim_publisher(out, a); });
    refused([&] { io::IndexLock manual(out, io::LockMode::exclusive, io::LockTarget::writer); });
    check(shin::context::read_file(out / "docs/kb/generated/manifest.json") == "new", "Snapshot lost during ownership checks");
}
}

int main() {
    try { run(); std::cout << "publisher ownership checks passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
