#include "shin/windows/file.h"
#include <windows.h>
#include <aclapi.h>
#include <set>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include <cstring>
#include <system_error>
#include "../../../tests/windows_private_acl.h"

namespace fs = std::filesystem;
using File = shin::windows::ReadFile;
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
void rejects(const std::function<void()>& action) {
    try { action(); } catch (const std::exception&) { return; }
    throw std::runtime_error("expected file refusal");
}
void write(const fs::path& path, const std::string& text) {
    std::ofstream stream(path, std::ios::binary);
    stream << text;
    stream.close();
    require(!stream.fail(), "cannot create test file");
}
void junction(const fs::path& link, const fs::path& target) {
    wchar_t system[MAX_PATH]{};
    require(GetSystemDirectoryW(system, MAX_PATH) != 0, "system directory unavailable");
    const auto executable = fs::path(system) / L"cmd.exe";
    auto command = L"cmd.exe /d /c mklink /J \"" + link.native() + L"\" \"" + target.native() + L"\"";
    STARTUPINFOW startup{}; startup.cb = sizeof(startup);
    PROCESS_INFORMATION child{};
    require(CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                           nullptr, nullptr, &startup, &child) != 0, "cannot launch junction fixture");
    const auto waited = WaitForSingleObject(child.hProcess, 10000);
    if (waited != WAIT_OBJECT_0) TerminateProcess(child.hProcess, 1);
    DWORD code = 1;
    GetExitCodeProcess(child.hProcess, &code);
    CloseHandle(child.hThread); CloseHandle(child.hProcess);
    require(waited == WAIT_OBJECT_0 && code == 0, "junction fixture failed");
}

std::set<fs::path> snapshot_files() {
    std::set<fs::path> result;
    const auto directory = fs::temp_directory_path();
    WIN32_FIND_DATAW entry{};
    const auto handle = FindFirstFileW((directory / L"shin-snapshot-*.tmp").c_str(), &entry);
    if (handle == INVALID_HANDLE_VALUE) {
        require(GetLastError() == ERROR_FILE_NOT_FOUND, "cannot enumerate owned snapshot fixtures");
        return result;
    }
    do { result.insert(directory / entry.cFileName); } while (FindNextFileW(handle, &entry));
    FindClose(handle);
    return result;
}

void owner_only_acl(const fs::path& path) {
    PSID owner = nullptr;
    PACL acl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    require(GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
        &owner, nullptr, &acl, nullptr, &descriptor) == ERROR_SUCCESS, "cannot inspect snapshot ACL");
    struct Cleanup { PSECURITY_DESCRIPTOR value; ~Cleanup() { LocalFree(value); } } cleanup{descriptor};
    SECURITY_DESCRIPTOR_CONTROL control{};
    DWORD revision = 0;
    require(GetSecurityDescriptorControl(descriptor, &control, &revision) && (control & SE_DACL_PROTECTED) &&
            acl && acl->AceCount == 1, "snapshot ACL is inherited or shared");
    void* raw = nullptr;
    require(GetAce(acl, 0, &raw) != 0, "snapshot ACE missing");
    const auto* ace = static_cast<ACCESS_ALLOWED_ACE*>(raw);
    require(ace->Header.AceType == ACCESS_ALLOWED_ACE_TYPE && ace->Mask == FILE_ALL_ACCESS &&
            EqualSid(owner, const_cast<DWORD*>(&ace->SidStart)), "snapshot ACL grants another principal");
    HANDLE token = nullptr;
    require(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) != 0, "process token unavailable");
    struct Close { HANDLE value; ~Close() { CloseHandle(value); } } close{token};
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    require(size >= sizeof(TOKEN_USER) && size <= 65536, "invalid token size");
    std::vector<unsigned char> user(size);
    require(GetTokenInformation(token, TokenUser, user.data(), size, &size) &&
        EqualSid(owner, reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid), "snapshot owner is not this process user");
}

DWORD cache_child(const fs::path& root, const wchar_t* mode = L"--probe-cache") {
    std::vector<wchar_t> executable(32768);
    const auto length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    require(length > 0 && length < executable.size(), "test executable path unavailable");
    auto command = L"\"" + std::wstring(executable.data(), length) + L"\" " + mode + L" \"" + root.native() + L"\"";
    STARTUPINFOW startup{}; startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    require(CreateProcessW(executable.data(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
        nullptr, nullptr, &startup, &process) != 0, "cache child launch failed");
    const auto wait = WaitForSingleObject(process.hProcess, 10000);
    if (wait != WAIT_OBJECT_0) TerminateProcess(process.hProcess, 1);
    DWORD code = 1;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hThread); CloseHandle(process.hProcess);
    require(wait == WAIT_OBJECT_0, "cache child timed out");
    return code;
}

int main(int argc, char** argv) {
    try {
        if (argc == 3) {
            const auto directory = fs::path(std::u8string(argv[2], argv[2] + std::strlen(argv[2])));
            try {
                shin::windows::PrivateCache cache(directory, "0123456789abcdef");
                if (std::string_view(argv[1]) == "--interrupt-cache") {
                    cache.save("published-before-interruption");
                    const auto pending = directory / L"0123456789abcdef.pending";
                    write(pending, "interrupted"); fixture_private_acl(pending);
                    ExitProcess(73); // Kernel must release the lock without C++ destructors.
                }
                require(std::string_view(argv[1]) == "--probe-cache", "unknown child mode");
            }
            catch (const std::system_error& error) {
                if (error.code().category() == std::system_category() && error.code().value() == ERROR_SHARING_VIOLATION) return 9;
                throw;
            }
            return 0;
        }
        const auto root = fs::temp_directory_path() / (L"shin-file-" + std::to_wstring(GetCurrentProcessId()) +
                                          L"-" + std::to_wstring(GetTickCount64()));
        require(fs::create_directory(root), "temporary directory collision");
        struct Cleanup { fs::path root; ~Cleanup() {
            std::error_code ec; fs::remove_all(fs::path(L"\\\\?\\" + root.native()), ec);
        } } cleanup{root};
        const auto path = root / L"Report.txt";
        write(path, "bounded bytes");
        unsigned passed = 0;
        const auto test = [&](const char* name, const std::function<void()>& action) {
            action(); ++passed; std::cout << "PASS " << name << '\n';
        };
        test("bounded repeatable read and native identity", [&] {
            File file(path);
            require(file.read(13) == "bounded bytes" && file.read(13) == "bounded bytes", "read mismatch");
            const auto stamp = file.stamp();
            require(stamp.bytes == 13 && stamp.file != 0 && stamp.volume != 0 && stamp.modified_ns > 0 &&
                    stamp.changed_ns > 0, "missing native identity");
            bool oversized = false;
            try { (void)file.read(12); } catch (const std::length_error&) { oversized = true; }
            require(oversized, "size limit bypassed");
        });
        test("empty file", [&] { write(root / L"empty", ""); require(File(root / L"empty").read(0).empty(), "empty read"); });
        test("Unicode and case aliases", [&] {
            require(File(path).stamp() == File(root / L"report.txt").stamp(), "case alias identity");
            const auto unicode = root / L"\u041f\u0440\u043e\u0435\u043a\u0442 \u6e2c\u8a66.txt";
            write(unicode, "unicode path"); require(File(unicode).read(32) == "unicode path", "Unicode path");
        });
        test("path and stream refusal", [&] {
            for (const auto& bad : {fs::path(L"relative.txt"), root / L"../Report.txt", root / L"./Report.txt",
                    root / L"Report.txt:stream", root / L"Report.txt.", root / L"Report.txt ", root / L"?",
                    fs::path(L"\\\\server\\share\\file"), fs::path(L"\\\\.\\NUL")})
                rejects([&] { File file(bad); });
            rejects([&] { File file(root); });
            rejects([&] { File file(root / L"missing"); });
        });
        test("live writer and replacement excluded", [&] {
            File file(path);
            const auto writer = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                             nullptr, OPEN_EXISTING, 0, nullptr);
            if (writer != INVALID_HANDLE_VALUE) CloseHandle(writer);
            require(writer == INVALID_HANDLE_VALUE, "writer admitted during read");
            require(!MoveFileW(path.c_str(), (root / L"renamed").c_str()), "file moved during read");
            require(!MoveFileW(root.c_str(), (root.native() + L"-moved").c_str()), "parent moved during read");
            require(file.read(64) == "bounded bytes", "pinned read changed");
        });
        test("preexisting writer refused", [&] {
            const auto writer = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                             nullptr, OPEN_EXISTING, 0, nullptr);
            require(writer != INVALID_HANDLE_VALUE, "fixture writer unavailable");
            bool refused = false;
            try { File file(path); } catch (const std::exception&) { refused = true; }
            CloseHandle(writer); require(refused, "existing writer ignored");
        });
        test("replacement gets a different identity", [&] {
            const auto before = File(path).stamp();
            const auto replacement = root / L"replacement";
            write(replacement, "bounded bytes");
            require(MoveFileExW(replacement.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0, "replace failed");
            require(File(path).stamp().file != before.file, "replacement identity collapsed");
        });
        test("junction refused at leaf and parent", [&] {
            const auto target = root / L"target", link = root / L"junction";
            fs::create_directory(target); write(target / L"source", "not exported"); junction(link, target);
            rejects([&] { File file(link); });
            rejects([&] { File file(link / L"source"); });
            require(fs::remove(link), "junction cleanup failed");
        });
        test("deep local path", [&] {
            auto deep = root;
            for (int i = 0; i < 8; ++i) {
                deep /= std::wstring(40, L'x'); fs::create_directory(fs::path(L"\\\\?\\" + deep.native()));
            }
            write(fs::path(L"\\\\?\\" + (deep / L"source").native()), "long path");
            require(File(deep / L"source").read(64) == "long path", "long path failed");
        });
        test("private file-backed read-only mapping and ACL", [&] {
            const auto before = snapshot_files();
            {
                shin::windows::PrivateMapping mapping(1024 * 1024);
                const auto files = snapshot_files();
                std::vector<fs::path> created;
                for (const auto& file : files) if (!before.contains(file)) created.push_back(file);
                require(created.size() == 1, "private snapshot file missing");
                owner_only_acl(created.front());
                mapping.append("prefix:");
                const std::string payload(180000, 'x');
                mapping.append(payload);
                rejects([&] { (void)mapping.view(0, 1); });
                mapping.seal();
                const auto view = mapping.view(7, payload.size());
                require(view == payload && mapping.view(0, 7) == "prefix:", "mapped bytes differ");
                MEMORY_BASIC_INFORMATION information{};
                require(VirtualQuery(view.data(), &information, sizeof(information)) == sizeof(information) &&
                        information.Protect == PAGE_READONLY && information.Type == MEM_MAPPED, "mapping is not read-only/file-backed");
                rejects([&] { mapping.append("late"); });
                rejects([&] { mapping.seal(); });
                rejects([&] { (void)mapping.view(180007, 1); });
                const auto reopened = CreateFileW(created.front().c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr, OPEN_EXISTING, 0, nullptr);
                if (reopened != INVALID_HANDLE_VALUE) CloseHandle(reopened);
                require(reopened == INVALID_HANDLE_VALUE, "sealed private backing remains externally readable");
            }
            require(snapshot_files() == before, "temporary backing leaked");
        });
        test("mapping budget, empty view and unsealed cleanup", [&] {
            const auto before = snapshot_files();
            rejects([&] { shin::windows::PrivateMapping mapping(0); });
            {
                shin::windows::PrivateMapping mapping(3);
                mapping.append("abc");
                bool oversized = false;
                try { mapping.append("d"); } catch (const std::length_error&) { oversized = true; }
                require(oversized, "mapping limit bypassed");
            }
            {
                shin::windows::PrivateMapping mapping;
                mapping.seal(); require(mapping.view(0, 0).empty(), "empty view");
            }
            require(snapshot_files() == before, "unsealed/empty backing leaked");
        });
        const std::string key = "0123456789abcdef";
        const auto cache_root = root / L"private-cache";
        const auto artifact = cache_root / (key + ".sqlite");
        test("private cache publication, bounds, reopen and process lock", [&] {
            {
                shin::windows::PrivateCache cache(cache_root, key);
                owner_only_acl(cache_root);
                require(!cache.load(64), "new cache not empty");
                cache.save("first"); owner_only_acl(artifact);
                require(cache.load(64) == "first" && !cache.load(4), "cache bytes/bound mismatch");
                require(cache_child(cache_root) == 9, "second process bypassed cache lock");
                require(!MoveFileW(cache_root.c_str(), (root / L"moved-cache").c_str()), "cache directory replaced while held");
                cache.save(std::string(180000, 'q'));
                require(cache.load(180000) == std::string(180000, 'q'), "large replacement mismatch");
                require(!fs::exists(cache_root / (key + ".pending")), "published pending remains");
            }
            require(cache_child(cache_root) == 0, "process lock was not released");
            shin::windows::PrivateCache cache(cache_root, key);
            require(cache.load(180000) == std::string(180000, 'q'), "reopened bytes differ");
        });
        test("cache ACL rejection without permission repair", [&] {
            fixture_private_acl(artifact, true);
            {
                shin::windows::PrivateCache cache(cache_root, key);
                rejects([&] { (void)cache.load(200000); });
                rejects([&] { cache.save("must not replace"); });
            }
            fixture_private_acl(artifact);
            fixture_private_acl(cache_root, true);
            rejects([&] { shin::windows::PrivateCache cache(cache_root, key); });
            fixture_private_acl(cache_root);
        });
        test("cache hard links and pending links refused", [&] {
            const auto original = cache_root / L"original";
            fs::rename(artifact, original);
            fs::create_hard_link(original, artifact);
            {
                shin::windows::PrivateCache cache(cache_root, key);
                rejects([&] { (void)cache.load(200000); });
                rejects([&] { cache.save("must not replace"); });
            }
            fs::remove(artifact); fs::rename(original, artifact);
            const auto pending = cache_root / (key + ".pending");
            fs::create_hard_link(artifact, pending);
            rejects([&] { shin::windows::PrivateCache cache(cache_root, key); });
            require(fs::exists(pending), "unsafe pending was deleted");
            fs::remove(pending);
        });
        test("recover only private pending and preserve unknown files", [&] {
            const auto pending = cache_root / (key + ".pending");
            write(pending, "partial"); fixture_private_acl(pending);
            write(cache_root / L"unrelated", "keep");
            shin::windows::PrivateCache cache(cache_root, key);
            require(!fs::exists(pending) && File(cache_root / L"unrelated").read(4) == "keep", "incorrect recovery scope");
            require(cache.load(180000) == std::string(180000, 'q'), "recovery damaged published cache");
        });
        test("cache junction directory and unsafe key refusal", [&] {
            const auto link = root / L"cache-junction";
            junction(link, cache_root);
            rejects([&] { shin::windows::PrivateCache cache(link, key); });
            rejects([&] { shin::windows::PrivateCache cache(link / L"child", key); });
            require(!fs::exists(cache_root / L"child"), "followed parent junction");
            fs::remove(link);
            rejects([&] { shin::windows::PrivateCache cache(cache_root, "../unsafe"); });
        });
        test("cache Unicode and long path", [&] {
            auto deep = root / L"\u041a\u0435\u0448";
            fs::create_directory(deep);
            for (int i = 0; i < 7; ++i) {
                deep /= std::wstring(40, L'y'); fs::create_directory(fs::path(L"\\\\?\\" + deep.native()));
            }
            shin::windows::PrivateCache cache(deep / L"cache", key);
            cache.save("long Unicode cache");
            require(cache.load(64) == "long Unicode cache", "long Unicode cache mismatch");
        });
        test("interrupted process releases lock and preserves publication", [&] {
            require(cache_child(cache_root, L"--interrupt-cache") == 73, "child did not interrupt");
            require(fs::exists(cache_root / (key + ".pending")), "crash fixture missing");
            shin::windows::PrivateCache cache(cache_root, key);
            require(cache.load(64) == "published-before-interruption" && !fs::exists(cache_root / (key + ".pending")),
                    "process interruption lost prior publication or left pending");
        });
        test("handles closed on success and refusal", [&] {
            DWORD before = 0, after = 0;
            require(GetProcessHandleCount(GetCurrentProcess(), &before) != 0, "handle count unavailable");
            for (int i = 0; i < 200; ++i) {
                File file(path); (void)file.read(64);
                rejects([&] { File missing(root / L"missing"); });
                shin::windows::PrivateMapping mapping(64);
                mapping.append("private"); mapping.seal();
                shin::windows::PrivateCache cache(cache_root, key);
                cache.save("cycle"); (void)cache.load(64);
            }
            require(GetProcessHandleCount(GetCurrentProcess(), &after) != 0 && after <= before, "handle leak");
        });
        std::cout << passed << " native Windows file checks passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; return 1; }
}
