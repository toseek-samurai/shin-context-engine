#include "shin/observer/io.h"
#include "shin/observer/local_identity.h"
#include "shin/observer/local_registry.h"
#include "shin/windows/file.h"
#include "../../tests/windows_private_acl.h"
#include <windows.h>
#include <fstream>
#include <iostream>
#include <thread>
#include <atomic>
#include <latch>
#include <algorithm>
#include <cstring>

using namespace shin::observer;
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class Action> void rejects(const char* code, Action action) {
    try { action(); } catch (const Error& error) {
        if (code && error.code != code) throw std::runtime_error(std::string("Expected ") + code + ", got " + error.code);
        return;
    }
    throw std::runtime_error("Expected storage refusal");
}
void file(const fs::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary); out << text; out.close();
    check(!out.fail(), "fixture write failed"); fixture_private_acl(path);
}
struct Fixture {
    fs::path root, state;
    Fixture() {
        root = fs::temp_directory_path() / ("shin-observer-win-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
        check(!fs::exists(root), "fixture collision"); ensure_private_directory(root);
        state = root / "state"; ensure_private_directory(state);
    }
    ~Fixture() { std::error_code error; fs::remove_all(fs::path(L"\\\\?\\" + root.native()), error); }
};
DWORD child_image(const fs::path& executable, const std::wstring& arguments) {
    auto command = L"\"" + executable.native() + L"\" " + arguments;
    STARTUPINFOW startup{}; startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    check(CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
        &startup, &process) != 0, "child create");
    const auto wait = WaitForSingleObject(process.hProcess, 15000);
    if (wait != WAIT_OBJECT_0) TerminateProcess(process.hProcess, 1);
    DWORD code = 1; GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hThread); CloseHandle(process.hProcess);
    check(wait == WAIT_OBJECT_0, "child timeout"); return code;
}
DWORD child(const std::wstring& arguments) {
    std::vector<wchar_t> executable(32768);
    const auto size = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    check(size && size < executable.size(), "child path");
    return child_image(fs::path(std::wstring(executable.data(), size)), arguments);
}
std::wstring quoted(const fs::path& path) { return L"\"" + path.native() + L"\""; }
void readers_and_writers() {
    Fixture f;
    StateStore reader(f.state, false);
    check(!reader.load() && !reader.writer_present(), "empty reader state");
    check(fs::is_empty(f.state), "reader created files");
    rejects("writer-unavailable", [&] { StateStore writer(f.state, true, {}, false); });
    check(fs::is_empty(f.state), "existing-only writer created lock");
    {
        StateStore writer(f.state, true);
        check(writer.writer_present() && reader.writer_present(), "writer not visible");
        check(child(L"--busy " + quoted(f.state)) == 0, "cross-process writer exclusion");
        writer.save({{"generation", 1}});
        check(reader.load()->at("generation") == 1, "reader cannot read active writer state");
        writer.save({{"generation", 2}});
        check(reader.load()->at("generation") == 2 && !fs::exists(f.state / "state.pending"), "state replacement");
        check(!MoveFileW(f.state.c_str(), (f.root / "moved").c_str()), "live directory was replaced");
    }
    check(!reader.writer_present(), "released writer still present");
    rejects("writer-required", [&] { reader.save({}); });
    rejects("writer-required", [&] { reader.recover_pending(); });
}
void recovery_and_bounds() {
    Fixture f;
    check(child(L"--crash " + quoted(f.state)) == 73, "crash fixture failed");
    StateStore reader(f.state, false);
    check(reader.load()->at("generation") == 1 && fs::exists(f.state / "state.pending"), "reader recovered pending");
    StateStore writer(f.state, true);
    check(!writer.recovered_pending(), "constructor recovered pending");
    rejects("state-pending-exists", [&] { writer.save({{"generation", 2}}); });
    writer.recover_pending(); check(writer.recovered_pending(), "explicit recovery absent");
    writer.save({{"generation", 2}});
    rejects("state-too-large", [&] { writer.save({{"large", std::string(kMaximumStateBytes, 'x')}}); });
    check(reader.load()->at("generation") == 2, "oversize overwrote old state");
    file(f.state / "state.json", "{\"x\":1,\"x\":2}");
    rejects("invalid-json", [&] { (void)reader.load(); });
    file(f.state / "state.json", std::string(kMaximumStateBytes + 1, 'x'));
    rejects("state-too-large", [&] { (void)reader.load(); });
    file(f.state / "state.pending", std::string(kMaximumStateBytes + 1, 'x'));
    rejects("state-too-large", [&] { writer.recover_pending(); });
    check(fs::exists(f.state / "state.pending"), "oversized pending removed");
}
void unsafe_entries() {
    Fixture f;
    const auto target = f.root / "target"; file(target, "{}");
    StateStore reader(f.state, false);
    fs::create_symlink(target, f.state / "state.json");
    rejects("state-unsafe", [&] { (void)reader.load(); });
    fs::remove(f.state / "state.json"); fs::create_hard_link(target, f.state / "state.json");
    rejects("state-unsafe", [&] { (void)reader.load(); });
    fs::remove(f.state / "state.json"); file(f.state / "state.json", "{}");
    fixture_private_acl(f.state / "state.json", true);
    rejects("state-unsafe", [&] { (void)reader.load(); });
    fixture_private_acl(f.state / "state.json");
    fixture_private_acl(f.state, true);
    rejects("directory-changed", [&] { (void)reader.load(); });
    rejects("unsafe-directory", [&] { StateStore other(f.state, false); });
    fixture_private_acl(f.state);
    fs::create_directory_symlink(f.state, f.root / "alias");
    rejects("unsafe-directory", [&] { (void)directory_identity(f.root / "alias"); });
    fs::create_hard_link(target, f.state / "state.pending");
    StateStore writer(f.state, true);
    rejects("state-unsafe", [&] { writer.recover_pending(); });
    check(fs::exists(f.state / "state.pending") && read_text(target, 4, false) == "{}", "unsafe pending altered");
}
void identities_and_archives() {
    Fixture f;
    const auto identity = directory_identity(f.state, true);
    check(identity.starts_with("win-volume-v1:") && directory_identity(f.root / "STATE", true) == identity, "case alias identity");
    check(write_private_archive(f.state / "archive", "original"), "archive create");
    check(!write_private_archive(f.state / "archive", "original"), "archive retry rewrote");
    rejects("archive-conflict", [&] { (void)write_private_archive(f.state / "archive", "changed"); });
    fs::rename(f.state, f.root / "retired"); ensure_private_directory(f.state);
    check(directory_identity(f.state) != identity, "replacement reused directory identity");
    rejects("directory-changed", [&] { StateStore changed(f.state, true, identity); });
    check(fs::is_empty(f.state), "identity mismatch wrote lock");
    check(digest("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "SHA256 differs");
    check(!current_executable_path().empty(), "image location");
    check(executing_image_identity().starts_with("pe-codeview-v1:"), "loaded image identity");
}
std::size_t codeview_offset(const std::string& bytes, const std::string& identity) {
    const auto start = identity.find(':') + 1;
    std::string signature = "RSDS";
    for (std::size_t i = 0; i < 32; i += 2)
        signature += static_cast<char>(std::stoul(identity.substr(start + i, 2), nullptr, 16));
    const auto offset = bytes.find(signature);
    check(offset != std::string::npos && bytes.find(signature, offset + 1) == std::string::npos, "unique fixture CodeView record");
    return offset;
}
template<class T> T fixture_record(const std::string& bytes, std::size_t offset) {
    check(offset <= bytes.size() && sizeof(T) <= bytes.size() - offset, "fixture PE bounds");
    T result{}; std::memcpy(&result, bytes.data() + offset, sizeof(result)); return result;
}
template<class T> void fixture_record(std::string& bytes, std::size_t offset, const T& value) {
    check(offset <= bytes.size() && sizeof(T) <= bytes.size() - offset, "fixture PE write bounds");
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}
void image_qualification() {
    Fixture f;
    const auto executable = current_executable_path();
    const auto identity = executing_image_identity();
    const auto bytes = read_text(executable, 128ULL * 1024ULL * 1024ULL, false);
    const auto offset = codeview_offset(bytes, identity);
    const auto copy = f.state / "probe.exe";
    file(copy, bytes);
    check(child_image(copy, L"--image-pin " + quoted(copy) + L" " + std::wstring(identity.begin(), identity.end())) == 0,
        "copied image without PDB or process pin failed");
    check(child_image(copy, L"--image-substitution " + quoted(copy)) == 0, "pre-pin image substitution accepted");
    fs::remove(copy); fs::remove(f.state / "retired.exe");
    const auto refuse = [&](const std::string& malformed) {
        file(copy, malformed); bool refused = false;
        try { shin::windows::ReadFile image(copy); (void)image.image_identity(); }
        catch (const std::runtime_error&) { refused = true; }
        check(refused, "invalid/ambiguous PE identity accepted");
    };
    auto malformed = bytes; malformed[offset] = 'X'; refuse(malformed);
    malformed = bytes; std::fill_n(malformed.begin() + static_cast<std::ptrdiff_t>(offset + 4), 16, '\0'); refuse(malformed);
    refuse("not a PE image");
    const auto dos = fixture_record<IMAGE_DOS_HEADER>(bytes, 0);
    const auto nt_offset = static_cast<std::size_t>(dos.e_lfanew);
    const auto nt = fixture_record<IMAGE_NT_HEADERS64>(bytes, nt_offset);
    auto no_debug = nt; no_debug.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG] = {};
    malformed = bytes; fixture_record(malformed, nt_offset, no_debug); refuse(malformed);
    std::size_t debug_offset = 0;
    const auto debug_rva = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG].VirtualAddress;
    const auto sections = nt_offset + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER) + nt.FileHeader.SizeOfOptionalHeader;
    for (std::size_t i = 0; i < nt.FileHeader.NumberOfSections; ++i) {
        const auto section = fixture_record<IMAGE_SECTION_HEADER>(bytes, sections + i * sizeof(IMAGE_SECTION_HEADER));
        if (debug_rva >= section.VirtualAddress && debug_rva - section.VirtualAddress < section.SizeOfRawData)
            debug_offset = section.PointerToRawData + static_cast<std::size_t>(debug_rva - section.VirtualAddress);
    }
    check(debug_offset != 0, "fixture debug table section");
    const auto count = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG].Size / sizeof(IMAGE_DEBUG_DIRECTORY);
    check(count >= 2, "fixture requires multiple debug entries");
    std::size_t cv_entry = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const auto at = debug_offset + i * sizeof(IMAGE_DEBUG_DIRECTORY);
        if (fixture_record<IMAGE_DEBUG_DIRECTORY>(bytes, at).Type == IMAGE_DEBUG_TYPE_CODEVIEW) cv_entry = at;
    }
    check(cv_entry != 0, "fixture CodeView entry");
    const auto entry = fixture_record<IMAGE_DEBUG_DIRECTORY>(bytes, cv_entry);
    auto beyond = entry; beyond.AddressOfRawData = nt.OptionalHeader.SizeOfImage - 4;
    malformed = bytes; fixture_record(malformed, cv_entry, beyond); refuse(malformed);
    malformed = bytes;
    fixture_record(malformed, cv_entry == debug_offset ? debug_offset + sizeof(IMAGE_DEBUG_DIRECTORY) : debug_offset, entry);
    refuse(malformed);
    DWORD handles_before = 0, handles_after = 0;
    check(GetProcessHandleCount(GetCurrentProcess(), &handles_before) != 0, "image handle count");
    for (int i = 0; i < 100; ++i) {
        shin::windows::ReadFile image(executable);
        check(image.image_identity() == identity, "repeated image identity drift");
    }
    check(GetProcessHandleCount(GetCurrentProcess(), &handles_after) != 0 && handles_after <= handles_before + 1,
        "image inspection leaked handles");
}
void user_archives() {
    Fixture f;
    fixture_private_acl(f.state, true, false, L"(A;OICIIO;FA;;;WD)(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)");
    const auto path = f.state / L"\u0410\u0440\u0445\u0438\u0432.json";
    const auto id = directory_identity(f.state);
    check(write_user_archive(path, "original"), "read-shared directory archive create");
    check(!write_user_archive(path, "original"), "user archive retry rewrote");
    rejects("archive-conflict", [&] { (void)write_user_archive(path, "different"); });
    rejects("archive-conflict", [&] { remove_user_archive(path, "different"); });
    rejects("archive-conflict", [&] { replace_user_archive(path, "different", "next", id); });
    rejects("unsafe-directory", [&] { replace_user_archive(path, "original", "next", "wrong"); });
    rejects("archive-too-large", [&] { replace_user_archive(path, "original", std::string(kMaximumStateBytes + 1, 'x'), id); });
    check(read_text(path, 32, true) == "original", "refusal changed user archive");
    fixture_private_acl(f.state, false, true);
    rejects("unsafe-directory", [&] { (void)write_user_archive(f.state / "other", "x"); });
    rejects("unsafe-directory", [&] { remove_user_archive(path, "original"); });
    rejects("unsafe-directory", [&] { replace_user_archive(path, "original", "next", id); });
    fixture_private_acl(f.state, true);
    fixture_private_acl(path, true);
    rejects("unsafe-file", [&] { replace_user_archive(path, "original", "next", id); });
    rejects("unsafe-file", [&] { remove_user_archive(path, "original"); });
    fixture_private_acl(path);
    fs::create_hard_link(path, f.root / "link");
    rejects("unsafe-file", [&] { replace_user_archive(path, "original", "next", id); });
    rejects("unsafe-file", [&] { remove_user_archive(path, "original"); });
    fs::remove(f.root / "link");
    check(SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_READONLY) != 0, "readonly fixture");
    rejects("unsafe-file", [&] { replace_user_archive(path, "original", "next", id); });
    check(SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL) != 0, "readonly reset");
    check(std::distance(fs::directory_iterator(f.state), fs::directory_iterator()) == 1, "failed replace leaked staged archive");
    check(read_text(path, 32, true) == "original", "failed commit removed original");
    replace_user_archive(path, "original", "next", id);
    replace_user_archive(path, "original", "next", id);
    check(read_text(path, 32, true) == "next", "user archive replace/retry");
    remove_user_archive(path, "next");
    check(fs::is_empty(f.state), "user archive removal left entries");
    rejects("unsafe-file", [&] { remove_user_archive(path, "next"); });
    const auto target = f.root / "target"; file(target, "preserve");
    fs::create_symlink(target, path);
    rejects("unsafe-file", [&] { (void)write_user_archive(path, "x"); });
    rejects("unsafe-file", [&] { replace_user_archive(path, "preserve", "next", id); });
    rejects("unsafe-file", [&] { remove_user_archive(path, "preserve"); });
    rejects("unsafe-file", [&] { (void)write_user_archive(f.state / "target:stream", "x"); });
    check(read_text(target, 32, true) == "preserve", "unsafe archive changed linked target");
}
void owner_rights_acl() {
    Fixture f;
    fixture_private_acl(f.state, false, false, L"(A;OICI;FA;;;OW)");
    const auto path = f.state / "owned-file";
    check(write_user_archive(path, "owned"), "OWNER RIGHTS directory was rejected");
    fixture_private_acl(path, false, false, L"(A;;FA;;;OW)");
    { shin::windows::ReadFile file(path); file.require_user_owned(); }
    rejects("unsafe-file", [&] { (void)read_text(path, 32, true); });
    fixture_private_acl(path, false, true, L"(A;;FA;;;OW)");
    bool denied = false;
    try { shin::windows::ReadFile file(path); file.require_user_owned(); }
    catch (const std::exception&) { denied = true; }
    check(denied, "OWNER RIGHTS hid a foreign writer");
    fixture_private_acl(f.state, false, true, L"(A;OICI;FA;;;OW)");
    rejects("unsafe-directory", [&] { (void)write_user_archive(f.state / "foreign", "no"); });
    check(!fs::exists(f.state / "foreign"), "foreign-write directory was mutated");
}
LocalProjectRequest prepare(const fs::path& home, const fs::path& root) {
    ensure_private_directory(home); ensure_private_directory(root);
    ensure_private_directory(home / "projects");
    const auto workspace = home / "projects" / digest(fs::canonical(root).string());
    ensure_private_directory(workspace);
    StateStore store(workspace, true);
    store.save({{"schema_version", "shin.local-project-binding.v1"}, {"root", fs::canonical(root).string()},
        {"root_identity", directory_identity(root)}, {"workspace_identity", directory_identity(workspace, true)},
        {"profile_dir", "fixture-profile"}, {"prepared_snapshot", "fixture-not-live-readiness"}});
    LocalProjectRequest request; request.home = home; request.root = root; return request;
}
void local_provenance() {
    Fixture f;
    const auto request = prepare(f.root / "home", f.root / "source");
    const auto first = local_identity(request, true);
    check(!first.at("authentication").get<bool>() && first.at("server_binding").is_null(), "identity grants remote access");
    check(first == local_identity(request, true) && first == local_identity(request, false), "identity not idempotent");
    const auto host = first.at("installation_id").get<std::string>();
    const auto workspace = first.at("local_workspace_id").get<std::string>();
    check(child(L"--identity " + quoted(request.home) + L" " + quoted(request.root) + L" " +
        std::wstring(host.begin(), host.end()) + L" " + std::wstring(workspace.begin(), workspace.end())) == 0, "process restart identity changed");
    const auto second_request = prepare(request.home, f.root / "second-source");
    const auto second = local_identity(second_request, true);
    check(second.at("installation_id") == first.at("installation_id") &&
        second.at("local_workspace_id") != first.at("local_workspace_id"), "different roots merged");
    enroll_local_project(request); enroll_local_project(second_request);
    check(registered_local_projects(request.home).size() == 2, "registry lost roots");
    const auto identity = request.home / "projects" / digest(fs::canonical(request.root).string()) / "identity" / "state.json";
    fs::remove(identity);
    rejects("local-identity-unavailable", [&] { (void)local_identity(request, true); });
    check(!fs::exists(identity), "lost workspace identity regenerated");
    fs::remove(request.home / "identity" / "state.json");
    rejects("local-identity-unavailable", [&] { (void)local_identity(second_request, true); });
}
void unicode_and_long_path() {
    Fixture f;
    auto path = f.root / L"\u041f\u0440\u043e\u0435\u043a\u0442";
    ensure_private_directory(path);
    for (int i = 0; i < 7; ++i) { path /= std::wstring(40, L'x'); ensure_private_directory(path); }
    const auto identity = directory_identity(path, true);
    { StateStore writer(path, true, identity); writer.save({{"text", "UTF-8"}}); }
    StateStore reader(path, false, identity); check(reader.load()->at("text") == "UTF-8", "long path state");
    const auto archive = path / L"\u0410\u0440\u0445\u0438\u0432";
    check(write_user_archive(archive, "one"), "long path archive");
    replace_user_archive(archive, "one", "two", identity);
    remove_user_archive(archive, "two");
}
void copied_and_substituted_identity() {
    Fixture f;
    const auto request = prepare(f.root / "home", f.root / "source");
    const auto original = local_identity(request, true);
    const auto copy = f.root / "copied-home";
    fs::copy(request.home, copy, fs::copy_options::recursive);
    fixture_private_acl(copy);
    for (const auto& entry : fs::recursive_directory_iterator(copy)) fixture_private_acl(entry.path());
    auto copied = request; copied.home = copy;
    rejects("local-identity-conflict", [&] { (void)local_identity(copied, true); });
    check(local_identity(request, false) == original, "copy refusal changed original identity");
    const auto identity = request.home / "projects" / digest(fs::canonical(request.root).string()) / "identity";
    Json saved;
    {
        StateStore store(identity, true); saved = *store.load();
        auto changed = saved; changed["local_workspace_id"] = "00000000-0000-4000-8000-000000000000"; store.save(changed);
    }
    rejects("local-identity-conflict", [&] { (void)local_identity(request, true); });
    { StateStore store(identity, true); store.save(saved); }
    fs::rename(request.root, f.root / "retired-source"); ensure_private_directory(request.root);
    rejects("local-identity-conflict", [&] { (void)local_identity(request, true); });
}
void concurrent_enrollment() {
    Fixture f;
    const auto first = prepare(f.root / "home", f.root / "a");
    const auto second = prepare(first.home, f.root / "b");
    std::latch start(3);
    Json a, b; std::exception_ptr errors[2];
    std::jthread one([&] { start.arrive_and_wait(); try { a = local_identity(first, true); } catch (...) { errors[0] = std::current_exception(); } });
    std::jthread two([&] { start.arrive_and_wait(); try { b = local_identity(second, true); } catch (...) { errors[1] = std::current_exception(); } });
    start.arrive_and_wait(); one.join(); two.join();
    for (const auto& error : errors) if (error) std::rethrow_exception(error);
    check(a.at("installation_id") == b.at("installation_id") && a.at("local_workspace_id") != b.at("local_workspace_id"),
        "concurrent roots lost shared installation or merged copies");
    check(local_identity(first, false) == a && local_identity(second, false) == b, "catalog commit lost a root");
}
void concurrent_state_reads() {
    Fixture f;
    StateStore writer(f.state, true);
    writer.save({{"generation", 0}, {"payload", std::string(1024, 'a')}});
    std::atomic<bool> finished{false}; std::exception_ptr error;
    std::jthread producer([&] {
        try {
            for (int i = 1; i <= 60; ++i) writer.save({{"generation", i}, {"payload", std::string(1024, static_cast<char>('a' + i % 20))}});
        } catch (...) { error = std::current_exception(); }
        finished = true;
    });
    StateStore reader(f.state, false);
    do {
        try {
            const auto value = reader.load(); check(value.has_value(), "committed state disappeared");
            const auto generation = value->at("generation").get<int>();
            check(value->at("payload") == std::string(1024, static_cast<char>('a' + generation % 20)), "mixed generation bytes");
        } catch (const Error& failure) { if (failure.code != "state-changed") { producer.join(); throw; } }
    } while (!finished);
    producer.join(); if (error) std::rethrow_exception(error);
    check(reader.load()->at("generation") == 60, "final generation missing");
}
void handle_lifecycle() {
    Fixture f;
    DWORD before = 0, after = 0; check(GetProcessHandleCount(GetCurrentProcess(), &before) != 0, "handle baseline");
    for (int i = 0; i < 200; ++i) {
        StateStore writer(f.state, true); writer.save({{"generation", i}});
        StateStore reader(f.state, false); check(reader.writer_present() && reader.load()->at("generation") == i, "lifecycle state");
        rejects("writer-busy", [&] { StateStore collision(f.state, true); });
    }
    check(GetProcessHandleCount(GetCurrentProcess(), &after) != 0 && after <= before, "storage handle leak");
}
void service_storage_boundaries() {
    Fixture f;
    ensure_local_registry(f.root);
    check(registered_local_projects(f.root).empty(), "service bootstrap enrolled a project");
    const auto original = read_text(f.root / "registry/state.json", kMaximumStateBytes, true);
    ensure_local_registry(f.root);
    check(read_text(f.root / "registry/state.json", kMaximumStateBytes, true) == original, "empty registry bootstrap is not idempotent");
    fs::remove(f.root / "registry/state.json");
    rejects("registry-unavailable", [&] { ensure_local_registry(f.root); });
    check(!fs::exists(f.root / "registry/state.json"), "missing authority was silently recreated");
    const auto image = f.root / "candidate.exe";
    fs::copy_file(current_executable_path(), image); fixture_private_acl(image);
    {
        shin::windows::ReadFile code(image); code.require_executable_owner();
        bool denied = false;
        try { code.require_machine_owned(); } catch (const std::exception&) { denied = true; }
        check(denied, "user-writable kit admitted as machine service");
    }
    fixture_private_acl(image, false, true);
    {
        shin::windows::ReadFile code(image); bool denied = false;
        try { code.require_executable_owner(); } catch (const std::exception&) { denied = true; }
        check(denied, "foreign-writable executable accepted");
    }
}
int main(int argc, char** argv) {
    try {
        if (argc >= 3) {
            const fs::path path(argv[2]);
            if (std::string_view(argv[1]) == "--machine-image" && argc == 3) {
                shin::windows::ReadFile image(path);
                image.require_machine_owned();
                check(!image.image_identity().empty(), "protected image identity missing");
                std::cout << "Protected machine executable and all ancestors accepted\n";
                return 0;
            }
            if (std::string_view(argv[1]) == "--image-pin" && argc == 4) {
                check(current_executable_path() == path && executing_image_identity() == argv[3], "process image identity changed");
                check(!MoveFileW(path.c_str(), (path.parent_path() / "moved.exe").c_str()), "pinned image renamed");
                check(!MoveFileW(path.parent_path().c_str(), (path.parent_path().parent_path() / "moved").c_str()), "pinned parent renamed");
                const auto write = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
                if (write != INVALID_HANDLE_VALUE) CloseHandle(write);
                check(write == INVALID_HANDLE_VALUE, "pinned image writable"); return 0;
            }
            if (std::string_view(argv[1]) == "--image-substitution") {
                std::string identity;
                { shin::windows::ReadFile image(path); identity = image.image_identity(); }
                auto bytes = read_text(path, 128ULL * 1024ULL * 1024ULL, false);
                const auto offset = codeview_offset(bytes, identity);
                bytes[offset + 4] ^= 1;
                fs::rename(path, path.parent_path() / "retired.exe");
                file(path, bytes);
                rejects("installation-unavailable", [] { (void)current_executable_path(); });
                rejects("installation-unavailable", [] { (void)executing_image_identity(); }); return 0;
            }
            if (std::string_view(argv[1]) == "--busy") {
                rejects("writer-busy", [&] { StateStore writer(path, true); }); return 0;
            }
            if (std::string_view(argv[1]) == "--crash") {
                StateStore writer(path, true); writer.save({{"generation", 1}});
                file(path / "state.pending", "interrupted"); ExitProcess(73);
            }
            if (std::string_view(argv[1]) == "--identity" && argc == 6) {
                LocalProjectRequest request; request.home = path; request.root = argv[3];
                const auto value = local_identity(request, false);
                check(value.at("installation_id") == argv[4] && value.at("local_workspace_id") == argv[5], "restart identity"); return 0;
            }
            return 2;
        }
        for (const auto& [label, run] : std::vector<std::pair<const char*, void(*)()>>{
            {"readers/writers", readers_and_writers}, {"recovery/bounds", recovery_and_bounds},
            {"unsafe entries", unsafe_entries}, {"directory identity/archives", identities_and_archives}, {"user archives", user_archives},
            {"OWNER RIGHTS ACL", owner_rights_acl},
            {"service storage boundaries", service_storage_boundaries},
            {"image qualification", image_qualification},
            {"local provenance", local_provenance}, {"Unicode/long paths", unicode_and_long_path},
            {"copied/substituted identity", copied_and_substituted_identity}, {"concurrent enrollment", concurrent_enrollment},
            {"concurrent reads", concurrent_state_reads}, {"handle lifecycle", handle_lifecycle}}) {
            std::cout << "RUN " << label << std::endl; run();
        }
        std::cout << "Windows Observer: 12 storage/identity/archive/image suites passed\n"; return 0;
    } catch (const Error& error) { std::cerr << error.code << ": " << error.what() << '\n'; return 1; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
