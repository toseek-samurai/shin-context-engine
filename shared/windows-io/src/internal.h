#pragma once
#include <windows.h>
#include <sddl.h>
#include <aclapi.h>
#include <system_error>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace shin::windows::detail {
class Handle {
public:
    explicit Handle(HANDLE value = INVALID_HANDLE_VALUE) : value_(value) {}
    ~Handle() { if (value_ && value_ != INVALID_HANDLE_VALUE) CloseHandle(value_); }
    Handle(Handle&& other) noexcept : value_(std::exchange(other.value_, INVALID_HANDLE_VALUE)) {}
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle& operator=(Handle&& other) noexcept {
        if (this != &other) {
            if (value_ && value_ != INVALID_HANDLE_VALUE) CloseHandle(value_);
            value_ = std::exchange(other.value_, INVALID_HANDLE_VALUE);
        }
        return *this;
    }
    HANDLE get() const { return value_; }
private:
    HANDLE value_;
};

[[noreturn]] inline void failed(const char* operation) {
    throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), operation);
}
inline void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct LocalAllocation {
    void* value = nullptr;
    explicit LocalAllocation(void* input) : value(input) {}
    ~LocalAllocation() { if (value) LocalFree(value); }
    LocalAllocation(const LocalAllocation&) = delete;
    LocalAllocation& operator=(const LocalAllocation&) = delete;
};

class PrivateSecurity {
public:
    PrivateSecurity() {
        HANDLE raw_token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw_token)) failed("Cannot identify private file owner");
        const Handle token(raw_token);
        DWORD length = 0;
        GetTokenInformation(token.get(), TokenUser, nullptr, 0, &length);
        require(length >= sizeof(TOKEN_USER) && length <= 65536, "Invalid process token size");
        user.resize(length);
        if (!GetTokenInformation(token.get(), TokenUser, user.data(), length, &length)) failed("Cannot identify private file owner");
        LPWSTR sid = nullptr;
        if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid, &sid)) failed("Cannot encode private file owner");
        const LocalAllocation sid_owner{sid};
        const std::wstring sddl = L"O:" + std::wstring(sid) + L"D:P(A;;FA;;;" + sid + L")";
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr))
            failed("Cannot create private file permissions");
    }
    ~PrivateSecurity() { if (descriptor) LocalFree(descriptor); }
    PrivateSecurity(const PrivateSecurity&) = delete;
    PrivateSecurity& operator=(const PrivateSecurity&) = delete;
    SECURITY_ATTRIBUTES attributes() const { return {sizeof(SECURITY_ATTRIBUTES), descriptor, FALSE}; }
    bool same_user(HANDLE token) const {
        DWORD size = 0;
        GetTokenInformation(token, TokenUser, nullptr, 0, &size);
        require(size >= sizeof(TOKEN_USER) && size <= 65536, "Invalid peer token size");
        std::vector<unsigned char> peer(size);
        if (!GetTokenInformation(token, TokenUser, peer.data(), size, &size)) failed("Cannot inspect peer token");
        return EqualSid(reinterpret_cast<const TOKEN_USER*>(user.data())->User.Sid,
                        reinterpret_cast<TOKEN_USER*>(peer.data())->User.Sid) != 0;
    }
    void verify(HANDLE handle) const {
        PSID owner = nullptr;
        PACL acl = nullptr;
        PSECURITY_DESCRIPTOR actual = nullptr;
        const auto status = GetSecurityInfo(handle, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                                            &owner, nullptr, &acl, nullptr, &actual);
        const LocalAllocation cleanup{actual};
        if (status != ERROR_SUCCESS) throw std::system_error(static_cast<int>(status), std::system_category(), "Cannot inspect cache ACL");
        SECURITY_DESCRIPTOR_CONTROL control{};
        DWORD revision = 0;
        require(GetSecurityDescriptorControl(actual, &control, &revision) && (control & SE_DACL_PROTECTED) &&
                owner && EqualSid(owner, reinterpret_cast<const TOKEN_USER*>(user.data())->User.Sid) &&
                acl && acl->AceCount == 1, "Private cache owner/ACL changed");
        void* raw = nullptr;
        require(GetAce(acl, 0, &raw) != 0, "Private cache ACE missing");
        const auto* ace = static_cast<ACCESS_ALLOWED_ACE*>(raw);
        require(ace->Header.AceType == ACCESS_ALLOWED_ACE_TYPE && ace->Header.AceFlags == 0 &&
                ace->Mask == FILE_ALL_ACCESS && EqualSid(owner, const_cast<DWORD*>(&ace->SidStart)),
                "Private cache grants another principal or inherited access");
    }
    void verify_user_directory(HANDLE handle) const { verify_owned(handle, false, false); }
    void verify_executable(HANDLE handle, bool machine_only = false) const { verify_owned(handle, true, machine_only); }
private:
    static bool machine_principal(PSID sid) {
        if (IsWellKnownSid(sid, WinLocalSystemSid) || IsWellKnownSid(sid, WinBuiltinAdministratorsSid)) return true;
        unsigned char trusted[SECURITY_MAX_SID_SIZE]{};
        wchar_t domain[256]{}; DWORD size = sizeof(trusted), count = 256; SID_NAME_USE use{};
        return LookupAccountNameW(nullptr, L"NT SERVICE\\TrustedInstaller", trusted, &size, domain, &count, &use) && EqualSid(sid, trusted);
    }
    void verify_owned(HANDLE handle, bool executable, bool machine_only) const {
        PSID owner = nullptr;
        PACL acl = nullptr;
        PSECURITY_DESCRIPTOR actual = nullptr;
        const auto status = GetSecurityInfo(handle, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                                            &owner, nullptr, &acl, nullptr, &actual);
        const LocalAllocation cleanup{actual};
        if (status != ERROR_SUCCESS) throw std::system_error(static_cast<int>(status), std::system_category(), "Cannot inspect user directory ACL");
        require(owner && ((!machine_only && EqualSid(owner, reinterpret_cast<const TOKEN_USER*>(user.data())->User.Sid)) ||
                (executable && machine_principal(owner))) &&
                acl && IsValidAcl(acl), "User directory must have an explicit DACL and current owner");
        for (DWORD i = 0; i < acl->AceCount; ++i) {
            void* raw = nullptr;
            require(GetAce(acl, i, &raw) != 0, "User directory ACE missing");
            const auto* header = static_cast<ACE_HEADER*>(raw);
            require(header->AceType == ACCESS_ALLOWED_ACE_TYPE || header->AceType == ACCESS_DENIED_ACE_TYPE,
                    "Unsupported user directory ACE");
            // Inherit-only rights do not apply to this directory. New archive
            // files have a protected DACL; existing files are verified separately.
            if (header->AceType == ACCESS_DENIED_ACE_TYPE || (header->AceFlags & INHERIT_ONLY_ACE)) continue;
            const auto* ace = static_cast<ACCESS_ALLOWED_ACE*>(raw);
            auto* sid = const_cast<DWORD*>(&ace->SidStart);
            // OWNER RIGHTS names the already-verified current owner, not another
            // account. Administrators/System remain outside this threat boundary.
            if (EqualSid(owner, sid) || IsWellKnownSid(sid, WinCreatorOwnerRightsSid) ||
                IsWellKnownSid(sid, WinLocalSystemSid) || IsWellKnownSid(sid, WinBuiltinAdministratorsSid) ||
                (executable && machine_principal(sid))) continue;
            auto mask = ace->Mask;
            GENERIC_MAPPING mapping{FILE_GENERIC_READ, FILE_GENERIC_WRITE, FILE_GENERIC_EXECUTE, FILE_ALL_ACCESS};
            MapGenericMask(&mask, &mapping);
            require((mask & ~(FILE_GENERIC_READ | FILE_GENERIC_EXECUTE)) == 0,
                    "User directory grants another principal write or ownership access");
        }
    }
private:
    std::vector<unsigned char> user;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
};

}
