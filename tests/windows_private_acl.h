#pragma once
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#include <filesystem>
#include <stdexcept>
#include <vector>

// Explicit fixture ACLs; std::filesystem::permissions cannot represent NTFS DACLs.
inline void fixture_private_acl(const std::filesystem::path& path, bool shared = false, bool writable = false,
                                const wchar_t* extra = L"") {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) throw std::runtime_error("fixture token");
    struct Token { HANDLE handle; ~Token() { CloseHandle(handle); } } close{token};
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    if (size == 0 || size > 65536) throw std::runtime_error("fixture token length");
    std::vector<unsigned char> data(size);
    if (!GetTokenInformation(token, TokenUser, data.data(), size, &size)) throw std::runtime_error("fixture token user");
    LPWSTR sid = nullptr;
    if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(data.data())->User.Sid, &sid))
        throw std::runtime_error("fixture SID");
    struct Local { void* value; ~Local() { LocalFree(value); } } sid_cleanup{sid};
    const auto sddl = L"D:P(A;;FA;;;" + std::wstring(sid) + L")" +
        (writable ? L"(A;;FA;;;WD)" : shared ? L"(A;;FR;;;WD)" : L"") + extra;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr))
        throw std::runtime_error("fixture ACL");
    Local cleanup{descriptor};
    PACL acl = nullptr;
    BOOL present = FALSE, defaulted = FALSE;
    if (!GetSecurityDescriptorDacl(descriptor, &present, &acl, &defaulted) || !present || !acl ||
        SetNamedSecurityInfoW(const_cast<LPWSTR>(path.c_str()), SE_FILE_OBJECT,
            DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr, acl, nullptr) != ERROR_SUCCESS)
        throw std::runtime_error("fixture set ACL");
}
