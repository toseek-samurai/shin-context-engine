#pragma once
#include "shin/observer/local_supervision.h"
#include <windows.h>
#include <winsvc.h>
#include <userenv.h>
#include <sddl.h>
#include <chrono>
#include <utility>
#include <vector>

namespace shin::observer::scm {
inline constexpr wchar_t name[] = L"ShinObserverHost";
using Clock = std::chrono::steady_clock;
inline void require(bool value, const char* code, const char* message) { if (!value) throw Error(code, message); }
[[noreturn]] inline void failed(const char* operation) {
    const auto error = GetLastError();
    throw Error(error == ERROR_ACCESS_DENIED ? "service-elevation-required" : "service-manager-failed",
        std::string(operation) + " (Windows " + std::to_string(error) + ").");
}
struct Handle {
    HANDLE value = nullptr;
    explicit Handle(HANDLE input = nullptr) : value(input) {}
    Handle(Handle&& other) noexcept : value(std::exchange(other.value, nullptr)) {}
    Handle& operator=(Handle&& other) noexcept {
        if (this != &other) { close(); value = std::exchange(other.value, nullptr); } return *this;
    }
    ~Handle() { close(); }
    void close() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); value = nullptr; }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};
struct Service {
    SC_HANDLE value;
    explicit Service(SC_HANDLE input) : value(input) {}
    Service(Service&& other) noexcept : value(std::exchange(other.value, nullptr)) {}
    ~Service() { if (value) CloseServiceHandle(value); }
    Service(const Service&) = delete;
    Service& operator=(const Service&) = delete;
};
struct Token {
    Handle handle;
    std::vector<unsigned char> user;
    explicit Token(HANDLE supplied = nullptr) : handle(supplied) {
        if (!handle.value && !OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &handle.value)) failed("Cannot inspect process token");
        DWORD bytes = 0; GetTokenInformation(handle.value, TokenUser, nullptr, 0, &bytes);
        require(bytes >= sizeof(TOKEN_USER) && bytes < 65536, "service-account-unavailable", "Invalid token size.");
        user.resize(bytes);
        if (!GetTokenInformation(handle.value, TokenUser, user.data(), bytes, &bytes)) failed("Cannot identify service user");
    }
    PSID sid() const { return reinterpret_cast<const TOKEN_USER*>(user.data())->User.Sid; }
    std::string key() const {
        LPSTR raw = nullptr; if (!ConvertSidToStringSidA(sid(), &raw)) failed("Cannot encode user identity");
        const std::string result(raw); LocalFree(raw); return result;
    }
    void desktop() const {
        require(!IsWellKnownSid(sid(), WinLocalSystemSid) && !IsWellKnownSid(sid(), WinLocalServiceSid) &&
            !IsWellKnownSid(sid(), WinNetworkServiceSid), "service-account-denied", "Observer requires a desktop user token.");
        TOKEN_ELEVATION elevation{}; DWORD size = 0;
        if (!GetTokenInformation(handle.value, TokenElevation, &elevation, sizeof(elevation), &size)) failed("Cannot inspect user elevation");
        require(!elevation.TokenIsElevated, "service-account-denied", "Observer must not run elevated.");
    }
    fs::path profile_home() const {
        DWORD count = 0; GetUserProfileDirectoryW(handle.value, nullptr, &count);
        require(count && count < 32768, "service-account-unavailable", "User profile is unavailable.");
        std::vector<wchar_t> path(count);
        if (!GetUserProfileDirectoryW(handle.value, path.data(), &count)) failed("Cannot locate user profile");
        return fs::canonical(fs::path(path.data())) / ".shin-ce";
    }
    fs::path home() const { desktop(); return profile_home(); }
};
Service manager(DWORD access = SC_MANAGER_CONNECT);
Service open(SC_HANDLE scm, const std::wstring& service, DWORD access);
SERVICE_STATUS_PROCESS state(SC_HANDLE service);
void verify(SC_HANDLE service, const fs::path& executable, bool disabled = false);
void protected_image(const fs::path& executable);
void wait_stopped(SC_HANDLE service);
std::wstring command(const fs::path& executable);
std::string endpoint();
void log_failure(const char* code) noexcept;
void WINAPI host_main(DWORD argc, wchar_t** argv) noexcept;
Json host_request(const fs::path& executable, const char* action);
}
