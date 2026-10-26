#pragma once

#include <nlohmann/json.hpp>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include "validation_diagnostic.h"

namespace shin::observer {
using Json = nlohmann::json;
namespace fs = std::filesystem;
inline constexpr std::size_t kMaximumStateBytes = 256U * 1024U;

class Error : public std::runtime_error {
public:
    Error(std::string code, const std::string& message, const Json& detail = nullptr)
        : std::runtime_error(message), code(std::move(code)), diagnostic(bounded_failure_diagnostic(detail)) {}
    std::string code;
    Json diagnostic;
};

inline Json error_json(const Error& error) {
    Json result{{"code", error.code}, {"message", error.what()}};
    if (!error.diagnostic.is_null()) result["diagnostic"] = error.diagnostic;
    return result;
}

std::string read_text(const fs::path& path, std::size_t maximum_bytes, bool private_only);
fs::path current_executable_path();
std::string executing_image_identity();
Json parse_json(const std::string& text);
// macOS: persistent volume UUID + inode + birth time; no boot-local fallback.
// Windows: local NTFS volume GUID + file ID + creation time; no drive-letter ID.
std::string directory_identity(const fs::path& path, bool private_only = false);
// Create one private component under an existing non-symlink parent.
// Exclusive creation refuses every existing entry, including an owned directory.
void ensure_private_directory(const fs::path& path, bool exclusive = false);
bool write_private_archive(const fs::path& path, const std::string& content);
// Private file in a user-owned directory which may be readable by other users.
bool write_user_archive(const fs::path& path, const std::string& content);
void remove_user_archive(const fs::path& path, const std::string& expected_content,
                         std::size_t maximum_bytes = kMaximumStateBytes);
// Caller serializes writers. Publish complete bytes, never a truncated live file.
void replace_user_archive(const fs::path& path, const std::string& expected_content,
                          const std::string& content, const std::string& parent_identity);

// Same optimistic atomic replacement, accepting owned 0600/0644 config files
// and preserving their permission bits. Symlinks/hardlinks remain refused.
// Windows currently admits only the owner-private ACL used by profile relocation.
void replace_user_config(const fs::path& path, const std::string& expected_content,
                         const std::string& content, const std::string& parent_identity);

class StateStore {
public:
    StateStore(fs::path directory, bool writer, const std::string& expected_identity = {}, bool create_lock = true);
    ~StateStore();
    StateStore(const StateStore&) = delete;
    StateStore& operator=(const StateStore&) = delete;
    std::optional<Json> load() const;
    void save(const Json& state);
    void recover_pending();
    bool writer_present() const;
    bool recovered_pending() const;
private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace shin::observer
