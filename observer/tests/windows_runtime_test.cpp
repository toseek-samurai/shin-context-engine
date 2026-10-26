#include "shin/observer/local_publication.h"
#include "shin/observer/local_rpc.h"
#include "shin/windows/file.h"
#include "shin/windows/transport.h"
#include "context_internal.h"
#include "generated_io.h"
#include "../../tests/core/windows_private_acl.h"
#include <windows.h>
#include <array>
#include <atomic>
#include <fstream>
#include <future>
#include <iostream>
#include <thread>

using namespace shin::observer;
namespace native = shin::windows;
namespace core = shin::context;
namespace io = shin::generated_io;
using Clock = std::chrono::steady_clock;
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
auto deadline(int ms = 3000) { return Clock::now() + std::chrono::milliseconds(ms); }
template<class Action> void refused(Action action, const std::string& code = {}) {
    try { action(); }
    catch (const native::TransportError& error) { check(code.empty() || error.code == code, "wrong transport refusal"); return; }
    catch (const Error& error) { check(code.empty() || error.code == code, "wrong RPC refusal"); return; }
    catch (const std::runtime_error&) { check(code.empty(), "unexpected untyped refusal"); return; }
    throw std::runtime_error("Expected native refusal");
}
struct Fixture {
    fs::path root;
    Fixture() : root(fs::canonical(fs::temp_directory_path()) / ("shin-runtime-win-" + std::to_string(GetCurrentProcessId()) + "-" +
        std::to_string(Clock::now().time_since_epoch().count()))) { ensure_private_directory(root); }
    ~Fixture() { std::error_code ignored; fs::remove_all(root, ignored); }
};
void file(const fs::path& path, const std::string& bytes) { std::ofstream out(path, std::ios::binary); out << bytes; out.close(); check(!out.fail(), "fixture file"); }
Json request(const std::string& id, const std::string& root = "fixture-root") {
    auto value = local_rpc::reply(id);
    value.update({{"command", "status"}, {"root", root}, {"query", ""}, {"path", ""}, {"record", ""}, {"snapshot", ""}});
    return value;
}
std::string endpoint(const std::string& suffix) { return digest(std::to_string(GetCurrentProcessId()) + suffix); }
void protocol() {
    auto input = request("id"); local_rpc::validate_request(input);
    input["command"] = "init"; refused([&] { local_rpc::validate_request(input); }, "service-method-denied");
    input = request("id"); input["observer_version"] = "old";
    refused([&] { local_rpc::validate_request(input); }, "service-version-mismatch");
    input = request("id"); input["extra"] = true;
    refused([&] { local_rpc::validate_request(input); }, "service-request-invalid");
    auto result = local_rpc::reply("id"); result.update({{"ok", true}, {"result", {{"ok", true}}}});
    check(local_rpc::project_response(result, "id").at("ok") == true, "RPC projection");
    refused([&] { (void)local_rpc::project_response(result, "other"); }, "service-response-invalid");
}
void pipes() {
    for (const bool pending : {false, true}) {
        std::cout << "RUN abandoned connection pending=" << pending << std::endl;
        const auto key = endpoint(pending ? "aborted-pending-accept" : "aborted-before-accept");
        native::PipeListener listener(key);
        if (pending) check(!listener.try_accept(), "listener unexpectedly connected");
        auto abandoned = native::Channel::connect(key, deadline());
        abandoned.close();
        if (auto closed = listener.try_accept()) {
            const auto data = closed->read_some(deadline());
            check(data && data->empty(), "abandoned client returned a request");
        }
        check(!listener.try_accept(), "listener did not rearm after abandoned client");
        auto healthy = native::Channel::connect(key, deadline());
        auto accepted = listener.accept(deadline());
        healthy.write_frame("still-alive", 8192, deadline());
        check(accepted.read_frame(8192, deadline()) == "still-alive", "abandoned client lost listener");
    }
    const auto key = endpoint("pipe"); native::PipeListener listener(key);
    refused([&] { native::PipeListener duplicate(key); }, "service-endpoint-busy");
    check(!listener.try_accept() && !listener.try_accept(), "idle listener did not remain pending");
    {
        std::vector<native::Channel> clients;
        std::vector<std::unique_ptr<native::Channel>> servers;
        for (int i = 0; i < 9; ++i) {
            clients.push_back(native::Channel::connect(key, deadline()));
            servers.push_back(std::make_unique<native::Channel>(listener.accept(deadline())));
        }
        servers.pop_back(); clients.pop_back();
        check(!listener.try_accept(), "capacity handoff lost its spare listener");
    }
    {
        auto connecting = std::async(std::launch::async, [&] { return listener.accept(deadline()); });
        auto client = native::Channel::connect(key, deadline()); auto server = connecting.get();
        const std::string payload(60000, 'w');
        auto reading = std::async(std::launch::async, [&] { return client.read_frame(128 * 1024, deadline()); });
        auto pending = payload + '\n';
        const auto until = deadline();
        while (!pending.empty()) {
            check(Clock::now() < until, "stream write deadline");
            if (const auto count = server.write_some(pending)) {
                check(*count > 0, "zero stream write"); pending.erase(0, *count);
            } else (void)server.wait_written(std::min(until, deadline(20)));
        }
        check(reading.get() == payload && !server.reply_consumed(), "stream reply was lost or acknowledged early");
        client.close(); check(server.reply_consumed(), "reply closure not observed");
    }
    {
        auto connecting = std::async(std::launch::async, [&] { return listener.accept(deadline()); });
        auto client = native::Channel::connect(key, deadline()); auto server = connecting.get();
        check(!server.write_some(std::string(65536, 'x')), "blocked stream unexpectedly completed");
        check(!server.wait_written(deadline(20)), "unread stream unexpectedly drained");
        refused([&] { (void)server.write_some("changed"); }, "service-frame-invalid");
        server.close(); client.close();
    }
    for (int i = 0; i < 8; ++i) {
        auto server = std::async(std::launch::async, [&] {
            try {
                auto channel = listener.accept(deadline());
                const auto input = parse_json(channel.read_frame(8192, deadline()));
                local_rpc::validate_request(input);
                auto response = local_rpc::reply(input.at("request_id"));
                response.update({{"ok", true}, {"result", {{"ok", true}, {"root", input.at("root")}}}});
                channel.write_frame(response.dump(), 128 * 1024, deadline());
                channel.finish_reply(deadline());
            } catch (const std::exception& error) { std::cerr << "PIPE server: " << error.what() << std::endl; throw; }
        });
        auto client = native::Channel::connect(key, deadline());
        const auto root = "project-" + std::to_string(i % 2);
        client.write_frame(request("id", root).dump(), 8192, deadline());
        const auto result = local_rpc::project_response(parse_json(client.read_frame(128 * 1024, deadline())), "id");
        check(result.at("root") == root, "transport mixed project envelopes");
        client.close(); server.get();
    }
    std::vector<std::future<void>> clients;
    for (int i = 0; i < 8; ++i) clients.push_back(std::async(std::launch::async, [&, i] {
        auto client = native::Channel::connect(key, deadline());
        const auto id = std::to_string(i);
        client.write_frame(request(id).dump(), 8192, deadline());
        const auto value = local_rpc::project_response(parse_json(client.read_frame(128 * 1024, deadline())), id);
        check(value.at("ok") == true, "concurrent pipe reply"); client.close();
    }));
    for (int i = 0; i < 8; ++i) {
        auto channel = listener.accept(deadline());
        const auto input = parse_json(channel.read_frame(8192, deadline())); local_rpc::validate_request(input);
        auto response = local_rpc::reply(input.at("request_id")); response.update({{"ok", true}, {"result", {{"ok", true}}}});
        channel.write_frame(response.dump(), 128 * 1024, deadline()); channel.finish_reply(deadline());
    }
    for (auto& client : clients) client.get();
    refused([&] { (void)listener.accept(deadline(30)); }, "service-timeout");
    refused([&] { (void)listener.accept(deadline(), [] { return true; }); }, "service-request-cancelled");
    auto accepting = std::async(std::launch::async, [&] { return listener.accept(deadline()); });
    auto client = native::Channel::connect(key, deadline()); auto server = accepting.get();
    for (int i = 0; i < 3; ++i) check(!server.wait_input(deadline(20)), "empty input signalled ready");
    client.write_frame("retained-first-byte", 8192, deadline());
    check(server.wait_input(deadline()) && server.wait_input(deadline()), "readiness lost buffered byte");
    check(server.read_frame(8192, deadline()) == "retained-first-byte", "readiness corrupted frame");
    check(!server.wait_input(deadline(20)), "old readiness leaked to next frame");
    refused([&] { (void)server.wait_input(deadline(), [] { return true; }); }, "service-request-cancelled");
    client.write_frame("", 8192, deadline());
    check(server.read_frame(8192, deadline()).empty(), "pending readiness lost empty frame");
    refused([&] { (void)server.read_frame(8192, deadline(30)); }, "service-timeout");
    refused([&] { client.write_frame("a\nb", 8192, deadline()); }, "service-frame-invalid");
    refused([&] { client.write_frame(std::string(8192, 'x'), 8192, deadline()); }, "service-frame-too-large");
    client.write_frame(std::string(40, 'x'), 8192, deadline());
    refused([&] { (void)server.read_frame(16, deadline()); }, "service-frame-too-large");
    client.close(); server.close();
    {
        auto connection = std::async(std::launch::async, [&] { return listener.accept(deadline()); });
        auto sender = native::Channel::connect(key, deadline()); auto receiver = connection.get();
        for (int i = 0; i < 3; ++i) check(!receiver.read_some(deadline(10)), "empty stream completed");
        const std::string payload(9000, 's');
        auto sending = std::async(std::launch::async, [&] { sender.write_frame(payload, 16384, deadline()); });
        std::string received;
        const auto until = deadline();
        while (received.find('\n') == std::string::npos) {
            check(Clock::now() < until, "stream receive deadline");
            if (const auto bytes = receiver.read_some(until)) {
                check(!bytes->empty() && bytes->size() <= 4096, "invalid stream chunk");
                received += *bytes;
            }
        }
        sending.get(); check(received == payload + '\n', "stream lost or repeated bytes");
        check(!receiver.read_some(deadline(10)), "stream replayed previous bytes");
        sender.close();
        const auto closed = receiver.read_some(deadline());
        check(closed && closed->empty(), "pending stream did not observe closure");
        receiver.close();
    }
    for (int i = 0; i < 20; ++i) {
        auto connection = std::async(std::launch::async, [&] { return listener.accept(deadline()); });
        auto sender = native::Channel::connect(key, deadline()); auto receiver = connection.get();
        check(!receiver.wait_input(deadline(1)), "unexpected pending input");
        if (i % 2 == 0) {
            sender.close();
            refused([&] { (void)receiver.wait_input(deadline()); }, "service-disconnected");
        }
        receiver.close();
    }
    {
        auto connection = std::async(std::launch::async, [&] { return listener.accept(deadline()); });
        const auto name = L"\\\\.\\pipe\\shin-local-v1-" + std::wstring(key.begin(), key.end());
        const auto sender = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
            SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
        check(sender != INVALID_HANDLE_VALUE, "fragment sender");
        struct Close { HANDLE value; ~Close() { CloseHandle(value); } } close{sender};
        auto receiver = connection.get();
        check(!receiver.wait_input(deadline(20)), "fragment input not empty");
        DWORD written = 0;
        check(WriteFile(sender, "par", 3, &written, nullptr) && written == 3, "first fragment");
        check(receiver.wait_input(deadline()), "first fragment not signalled");
        auto frame = std::async(std::launch::async, [&] { return receiver.read_frame(8192, deadline()); });
        check(frame.wait_for(std::chrono::milliseconds(100)) == std::future_status::timeout, "partial frame completed early");
        check(WriteFile(sender, "tial\n", 5, &written, nullptr) && written == 5, "final fragment");
        check(frame.get() == "partial", "partial frame lost bytes across readiness ticks");
    }
    refused([&] { (void)native::Channel::connect(endpoint("missing"), deadline()); }, "service-unavailable");
    // Default Windows pipe ACLs are deliberately too broad for a Shin endpoint.
    const auto unsafe = endpoint("unsafe"); const auto name = L"\\\\.\\pipe\\shin-local-v1-" + std::wstring(unsafe.begin(), unsafe.end());
    const auto handle = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        1, 8192, 8192, 0, nullptr);
    check(handle != INVALID_HANDLE_VALUE, "unsafe pipe fixture");
    refused([&] { (void)native::Channel::connect(unsafe, deadline()); }); CloseHandle(handle);
}
void publication() {
    Fixture f;
    const auto executable = f.root / "worker.exe";
    fs::copy_file(current_executable_path(), executable); fixture_private_acl(executable);
    ensure_private_directory(f.root / "home"); ensure_private_directory(f.root / "home/projects");
    std::array<std::string, 2> owners;
    for (std::size_t i = 0; i < 2; ++i) {
        const auto root = f.root / ("source" + std::to_string(i)); ensure_private_directory(root);
        file(root / "README.md", "# Project contract\n\nThis project owns a bounded runtime.\n");
        const auto workspace = f.root / "home/projects" / digest(root.string()); ensure_private_directory(workspace);
        const auto index = workspace / "index"; ensure_private_directory(index);
        core::RuntimeOptions options; options.root = root; options.out_root = index; options.profile_dir = SHIN_TEST_PROFILES;
        options.profile = core::make_profile("generic-docs", options.profile_dir);
        check(core::run_build(options) == 0, "native Core build");
        const auto snapshot = core::fnv1a64(core::read_file(index / "docs/kb/generated/manifest.json"));
        Config config; config.root = root; config.out_dir = index; config.profile = options.profile.id;
        config.profile_dir = options.profile_dir; config.max_snapshot_bytes = 128 * 1024 * 1024; config.max_file_bytes = 64 * 1024 * 1024;
        Json binding{{"schema_version", "shin.local-project-binding.v1"},
            {"root", root.string()}, {"root_identity", directory_identity(root)},
            {"workspace_identity", directory_identity(workspace, true)}, {"index_identity", directory_identity(index, true)},
            {"profile_config_hash", options.profile.config_hash}, {"marker", ""}, {"marker_hash", std::string(64, '0')}};
        StateStore writer(workspace, true);
        writer.save(binding);
        LocalPublication grant(workspace, binding, config);
        grant.enable(snapshot, options.profile.config_hash); grant.verify(); owners[i] = grant.token();
        check(!grant.reconcile(), "unchanged publication requires repair");
        refused([&] { (void)core::run_build(options); });
        check(core::fnv1a64(core::read_file(index / "docs/kb/generated/manifest.json")) == snapshot, "manual writer changed granted snapshot");
        auto moved = binding; moved["root"] = f.root.string();
        refused([&] { LocalPublication other(workspace, moved, config); }, "refresh-grant-changed");
        auto probe = request("worker", root.string());
        probe["query"] = index.string(); probe["record"] = grant.token();
        native::Worker child(executable, "_service-worker");
        child.input().write_frame(probe.dump(), 8192, deadline());
        const auto answer = local_rpc::project_response(parse_json(child.output().read_frame(128 * 1024, deadline(10000))), "worker");
        check(answer.at("root") == root.string() && answer.at("publisher") == true, "child cannot acquire exact publisher grant");
        check(child.wait(deadline()) == 0, "publisher worker failed");
        check(revoke_local_publication(workspace, binding).at("data_preserved") == true, "revoke lost data");
        refused([&] { grant.verify(); }, "refresh-authority-changed");
        check(!io::publisher_owner(index), "claim survived revoke");
    }
    check(owners[0] != owners[1], "two project grants merged");
}
std::string stdin_frame() {
    auto channels = native::Channel::worker_stdio();
    return channels.first.read_frame(8192, deadline());
}
void stdout_frame(const Json& value) {
    auto channels = native::Channel::worker_stdio();
    channels.second.write_frame(value.dump(), 128 * 1024, deadline());
    // The caller owns a deadline and can kill this job if the reader disappears.
    check(FlushFileBuffers(GetStdHandle(STD_OUTPUT_HANDLE)) != 0, "worker flush");
}
int worker(const std::string& role) {
    if (role == "_resident-worker") { WaitForSingleObject(GetCurrentProcess(), INFINITE); return 0; }
    const auto value = parse_json(stdin_frame());
    if (role == "_refresh-worker") {
        native::Worker descendant(current_executable_path(), "_resident-worker");
        stdout_frame({{"pid", descendant.process_id()}});
        if (value.value("crash", false)) { (void)stdin_frame(); ExitProcess(73); }
        (void)stdin_frame(); return 0;
    }
    local_rpc::validate_request(value);
    if (!value.at("query").get<std::string>().empty()) {
        io::IndexLock writer(value.at("query").get<std::string>(), io::LockMode::exclusive, io::LockTarget::writer, value.at("record"));
        check(io::publisher_owner(value.at("query").get<std::string>()) == value.at("record"), "cross-process publisher mismatch");
    }
    if (!value.at("path").get<std::string>().empty()) {
        const auto sentinel = reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(std::stoull(value.at("path").get<std::string>())));
        (void)SetEvent(sentinel);
    }
    std::array<wchar_t, 64> secret{};
    check(GetEnvironmentVariableW(L"SHIN_FIXTURE_SECRET", secret.data(), static_cast<DWORD>(secret.size())) == 0, "worker inherited unrelated environment");
    auto response = local_rpc::reply(value.at("request_id"));
    response.update({{"ok", true}, {"result", {{"ok", true}, {"root", value.at("root")}, {"publisher", !value.at("query").get<std::string>().empty()}}}});
    stdout_frame(response); return 0;
}
void workers() {
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), nullptr, TRUE};
    const auto sentinel = CreateEventW(&attributes, TRUE, FALSE, nullptr); check(sentinel != nullptr, "sentinel event");
    struct Close { HANDLE value; ~Close() { CloseHandle(value); } } close{sentinel};
    check(SetEnvironmentVariableW(L"SHIN_FIXTURE_SECRET", L"fixture-not-a-real-secret") != 0, "fixture environment");
    Fixture f;
    const auto unicode = f.root / L"\u041f\u0440\u043e\u0435\u043a\u0442 with spaces.exe";
    fs::copy_file(current_executable_path(), unicode); fixture_private_acl(unicode);
    fixture_private_acl(unicode, false, true);
    refused([&] { native::Worker unsafe(unicode, "_service-worker"); });
    fixture_private_acl(unicode);
    {
        native::Worker process(unicode, "_service-worker");
        auto value = request("test"); value["path"] = std::to_string(reinterpret_cast<std::uintptr_t>(sentinel));
        process.input().write_frame(value.dump(), 8192, deadline());
        check(local_rpc::project_response(parse_json(process.output().read_frame(128 * 1024, deadline())), "test").at("ok") == true, "worker roundtrip");
        check(process.wait(deadline()) == 0, "native worker exit");
    }
    SetEnvironmentVariableW(L"SHIN_FIXTURE_SECRET", nullptr);
    check(WaitForSingleObject(sentinel, 0) == WAIT_TIMEOUT, "unrelated inheritable handle leaked");
    for (const bool crash : {false, true}) {
        auto process = std::make_unique<native::Worker>(unicode, "_refresh-worker");
        process->input().write_frame(Json{{"crash", crash}}.dump(), 8192, deadline());
        const auto result = parse_json(process->output().read_frame(8192, deadline()));
        const auto descendant = OpenProcess(SYNCHRONIZE, FALSE, result.at("pid").get<DWORD>());
        check(descendant != nullptr, "descendant process handle"); Close held{descendant};
        if (crash) {
            process->input().write_frame("{}", 8192, deadline()); check(process->wait(deadline()) == 73, "crash fixture exit");
            check(WaitForSingleObject(descendant, 3000) == WAIT_OBJECT_0, "parent death did not close descendant job");
        }
        process.reset();
        check(WaitForSingleObject(descendant, 3000) == WAIT_OBJECT_0, "orphaned descendant escaped its job");
    }
    native::Worker blocked(unicode, "_resident-worker");
    refused([&] { blocked.input().write_frame(std::string(65536, 'x'), 128 * 1024, deadline(40)); }, "service-timeout");
    refused([&] { (void)blocked.wait(deadline(), [] { return true; }); }, "service-request-cancelled");
    check(blocked.wait(deadline()) != STILL_ACTIVE, "cancelled worker survived");
}
void handle_lifecycle() {
    DWORD before = 0, after = 0;
    check(GetProcessHandleCount(GetCurrentProcess(), &before) != 0, "initial handle count");
    for (int i = 0; i < 100; ++i) {
        const auto key = endpoint("lifecycle"); native::PipeListener listener(key);
        auto client = native::Channel::connect(key, deadline()); auto server = listener.accept(deadline());
        client.write_frame("{}", 8192, deadline()); check(server.read_frame(8192, deadline()) == "{}", "lifecycle frame");
        if (i % 2 == 0) { client.close(); server.finish_reply(deadline()); }
        else {
            check(!server.read_some(Clock::now()), "unexpected stream input at cleanup");
            server.close(); client.close();
        }
    }
    check(GetProcessHandleCount(GetCurrentProcess(), &after) != 0 && after <= before + 1, "pipe/event/token handle leak");
}
int main(int argc, char** argv) {
    try {
        if (argc == 2) return worker(argv[1]);
        for (const auto& [label, run] : std::array<std::pair<const char*, void(*)()>, 5>{{
            {"shared RPC", protocol}, {"private pipes", pipes}, {"publisher grants", publication}, {"owned workers", workers},
            {"handle lifecycle", handle_lifecycle}}}) {
            std::cout << "RUN " << label << std::endl; run();
        }
        std::cout << "Windows runtime adapters passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
