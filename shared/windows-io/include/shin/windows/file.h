#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <stdexcept>

namespace shin::windows {
// Local NTFS metadata, not a persistent installation/project identity.
struct FileStamp {
    std::uint64_t bytes = 0, volume = 0, file = 0;
    std::int64_t modified_ns = 0, changed_ns = 0;
    std::uint32_t links = 0, attributes = 0;
    bool operator==(const FileStamp&) const = default;
};

// Holds every path component against replacement. No data writes or
// symlink/junction traversal; writable opens conflict for this short lifetime.
class ReadFile {
public:
    explicit ReadFile(const std::filesystem::path& absolute);
    ~ReadFile();
    ReadFile(const ReadFile&) = delete;
    ReadFile& operator=(const ReadFile&) = delete;
    FileStamp stamp() const;
    void require_user_owned() const;
    // Code may be user-owned or installed by an administrator. Private data may not.
    void require_executable_owner() const;
    // SCM templates require an admin-protected executable and ancestor chain.
    void require_machine_owned() const;
    std::string read(std::size_t maximum_bytes);
    void stream(std::size_t maximum_bytes, const std::function<void(std::string_view)>& consume);
    // PE32+ CodeView identity, read via a non-executable Windows image mapping.
    std::string image_identity() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Compare the loaded PE with its named file, then pin the path for process life.
// Missing/mismatched build identity refuses; this is not signature verification.
std::filesystem::path running_image_path();
std::string running_image_identity();

// Anonymous read-only mapping over an owner-only, delete-on-close NTFS file.
// This is disposable snapshot storage, not the persistent Observer StateStore.
class PrivateMapping {
public:
    explicit PrivateMapping(std::size_t maximum_bytes = 1024ULL * 1024ULL * 1024ULL);
    ~PrivateMapping();
    PrivateMapping(const PrivateMapping&) = delete;
    PrivateMapping& operator=(const PrivateMapping&) = delete;
    void append(std::string_view bytes);
    void seal();
    std::string_view view(std::size_t offset, std::size_t length) const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// One disposable cache artifact with an exclusive cross-process writer handle.
// Not an authoritative identity/grant store or a power-loss durability promise.
class PrivateCache {
public:
    PrivateCache(const std::filesystem::path& directory, const std::string& key, bool recover_pending = true);
    ~PrivateCache();
    PrivateCache(const PrivateCache&) = delete;
    PrivateCache& operator=(const PrivateCache&) = delete;
    std::optional<std::string> load(std::size_t maximum_bytes) const;
    void save(std::string_view bytes) const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class Changed : public std::runtime_error {
public:
    Changed() : std::runtime_error("Named file changed during reading") {}
};
class WriterLock {
public:
    ~WriterLock();
    void check() const;
private:
    friend class Directory;
    struct Impl;
    explicit WriterLock(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

// Pinned local NTFS directory. Identity excludes drive letters and mount order.
class Directory {
public:
    enum class Access { read_only, private_owner, user_owned };
    explicit Directory(const std::filesystem::path& path, Access access = Access::read_only);
    ~Directory();
    static void ensure_private(const std::filesystem::path& path, bool exclusive = false);
    static void ensure_user_owned(const std::filesystem::path& path);
    std::string identity() const;
    void check() const;
    std::optional<std::string> read(const std::string& name, std::size_t limit, bool private_only = true) const;
    // SQLite owns file contents; these operations enforce private file metadata.
    std::optional<FileStamp> metadata(const std::string& name, std::size_t limit) const;
    // False means already present, not adopted; callers must inspect metadata.
    bool create_empty_file(const std::string& name) const;
    bool create(const std::string& name, std::string_view content) const;
    void publish(const std::string& name, const std::string& pending, std::string_view content) const;
    // Caller serializes writers. Replaces only the checked original (or an exact retry).
    void replace(const std::string& name, std::string_view expected, std::string_view content, std::size_t limit) const;
    bool remove(const std::string& name, std::size_t limit, const std::optional<std::string>& expected = {}) const;
    std::unique_ptr<WriterLock> writer(const std::string& name, bool create) const;
    std::unique_ptr<WriterLock> lock(const std::string& name, bool exclusive, bool create, bool wait) const;
    bool writer_present(const std::string& name) const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
