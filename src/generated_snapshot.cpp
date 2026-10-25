#include "generated_snapshot.h"

#include "context_internal.h"
#include "generated_io.h"
#include "snapshot_verification.h"

#ifdef _WIN32
#include "shin/windows/file.h"
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif
#include <array>
#include <cstdio>

namespace shin::context {
namespace {

class InputBuffer final : public std::streambuf {
public:
    explicit InputBuffer(std::string_view bytes) {
        // std::streambuf predates const input views; no output area is exposed.
        auto* begin = const_cast<char*>(bytes.data());
        setg(begin, begin, begin + bytes.size());
    }
protected:
    pos_type seekoff(off_type offset, std::ios_base::seekdir direction,
                     std::ios_base::openmode mode) override {
        if (mode != std::ios_base::in) return pos_type(off_type(-1));
        const auto size = static_cast<off_type>(egptr() - eback());
        off_type base = 0;
        if (direction == std::ios_base::cur) base = gptr() - eback();
        else if (direction == std::ios_base::end) base = size;
        else if (direction != std::ios_base::beg) return pos_type(off_type(-1));
        if (offset < -base || offset > size - base) return pos_type(off_type(-1));
        const auto position = base + offset;
        setg(eback(), eback() + position, egptr());
        return pos_type(position);
    }
    pos_type seekpos(pos_type position, std::ios_base::openmode mode) override {
        return seekoff(static_cast<off_type>(position), std::ios_base::beg, mode);
    }
};

#ifndef _WIN32
class Descriptor {
public:
    explicit Descriptor(int descriptor) : value_(descriptor) {}
    ~Descriptor() { if (value_ >= 0) ::close(value_); }
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
    int get() const noexcept { return value_; }
private:
    int value_;
};
#endif

std::string bounded_artifact(const fs::path& path, std::uintmax_t expected_bytes,
                             std::uintmax_t maximum_bytes) {
    if (expected_bytes > maximum_bytes || expected_bytes > std::numeric_limits<std::size_t>::max())
        throw CliError("snapshot-memory-budget-exceeded", "resource", "Snapshot artifact exceeds its memory budget.",
                       1, false, "Use a smaller snapshot or review the explicit load budget.");
#ifdef _WIN32
    try {
        windows::ReadFile input(path);
        if (input.stamp().bytes != expected_bytes)
            throw_index_error("snapshot-file-invalid", "Snapshot artifact is unavailable or changed.");
        return input.read(static_cast<std::size_t>(expected_bytes));
    } catch (const CliError&) { throw; }
    catch (const std::exception&) { throw_index_error("snapshot-file-invalid", "Snapshot artifact is unavailable or changed."); }
#else
    Descriptor descriptor(::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK));
    struct stat info{};
    if (descriptor.get() < 0 || ::fstat(descriptor.get(), &info) != 0 || !S_ISREG(info.st_mode) ||
        info.st_size < 0 || static_cast<std::uintmax_t>(info.st_size) != expected_bytes)
        throw_index_error("snapshot-file-invalid", "Snapshot artifact is unavailable or changed.");
    std::string bytes(static_cast<std::size_t>(expected_bytes), '\0');
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto count = ::read(descriptor.get(), bytes.data() + offset, bytes.size() - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) throw_index_error("snapshot-integrity-failed", "Snapshot artifact changed while loading.");
        offset += static_cast<std::size_t>(count);
    }
    char extra = 0;
    if (::read(descriptor.get(), &extra, 1) != 0)
        throw_index_error("snapshot-integrity-failed", "Snapshot artifact changed while loading.");
    return bytes;
#endif
}

} // namespace

#ifdef _WIN32
struct GeneratedSnapshot::Backing {
    windows::PrivateMapping storage;
    Backing() try : storage() {} catch (const std::exception&) {
        throw_index_error("snapshot-backing-unavailable", "Cannot create private temporary snapshot storage.");
    }
    void append(std::string_view bytes) {
        try { storage.append(bytes); }
        catch (const std::exception&) { throw_index_error("snapshot-backing-unavailable", "Cannot write private temporary snapshot storage."); }
    }
    std::string append(const fs::path& path, std::uintmax_t expected_bytes) {
        try {
            windows::ReadFile input(path);
            if (input.stamp().bytes != expected_bytes)
                throw_index_error("snapshot-file-invalid", "Snapshot artifact is unavailable or changed.");
            Fnv1a64Hasher hash;
            input.stream(static_cast<std::size_t>(expected_bytes), [&](std::string_view bytes) { hash.update(bytes); append(bytes); });
            return hash.digest();
        } catch (const CliError&) { throw; }
        catch (const std::exception&) { throw_index_error("snapshot-file-invalid", "Snapshot artifact is unavailable or changed."); }
    }
    void seal() {
        try { storage.seal(); }
        catch (const std::exception&) { throw_index_error("snapshot-backing-unavailable", "Cannot map private temporary snapshot storage."); }
    }
    std::string_view view(std::size_t offset, std::size_t length) const { return storage.view(offset, length); }
};
#else
struct GeneratedSnapshot::Backing {
    std::unique_ptr<std::FILE, decltype(&std::fclose)> file{std::tmpfile(), std::fclose};
    void* address = MAP_FAILED;
    std::size_t size = 0;

    Backing() {
        if (!file || ::fchmod(::fileno(file.get()), 0600) != 0 ||
            ::fcntl(::fileno(file.get()), F_SETFD, FD_CLOEXEC) != 0)
            throw_index_error("snapshot-backing-unavailable", "Cannot create private temporary snapshot storage.");
    }
    ~Backing() { if (address != MAP_FAILED) ::munmap(address, size); }
    void append(std::string_view bytes) {
        if (std::fwrite(bytes.data(), 1, bytes.size(), file.get()) != bytes.size())
            throw_index_error("snapshot-backing-unavailable", "Cannot write private temporary snapshot storage.");
        size += bytes.size();
    }
    std::string append(const fs::path& path, std::uintmax_t expected_bytes) {
        Descriptor input(::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK));
        struct stat info{};
        if (input.get() < 0 || ::fstat(input.get(), &info) != 0 || !S_ISREG(info.st_mode) ||
            info.st_size < 0 || static_cast<std::uintmax_t>(info.st_size) != expected_bytes)
            throw_index_error("snapshot-file-invalid", "Snapshot artifact is unavailable or changed.");
        std::array<char, 65536> buffer{};
        Fnv1a64Hasher hash;
        std::uintmax_t remaining = expected_bytes;
        while (remaining) {
            const auto count = ::read(input.get(), buffer.data(), std::min<std::uintmax_t>(remaining, buffer.size()));
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) throw_index_error("snapshot-integrity-failed", "Snapshot artifact changed while loading.");
            const auto bytes = std::string_view(buffer.data(), static_cast<std::size_t>(count));
            hash.update(bytes);
            append(bytes);
            remaining -= static_cast<std::uintmax_t>(count);
        }
        char extra = 0;
        if (::read(input.get(), &extra, 1) != 0)
            throw_index_error("snapshot-integrity-failed", "Snapshot artifact changed while loading.");
        return hash.digest();
    }
    void seal() {
        if (std::fflush(file.get()) != 0)
            throw_index_error("snapshot-backing-unavailable", "Cannot flush private temporary snapshot storage.");
        address = ::mmap(nullptr, size, PROT_READ, MAP_PRIVATE, ::fileno(file.get()), 0);
        if (address == MAP_FAILED)
            throw_index_error("snapshot-backing-unavailable", "Cannot map private temporary snapshot storage.");
        // The unlinked inode is private and immutable; no publisher can truncate it.
        // Closing the last descriptor leaves cleanup to the mapping's lifetime.
        file.reset();
    }
    std::string_view view(std::size_t offset, std::size_t length) const {
        return {static_cast<const char*>(address) + offset, length};
    }
};
#endif

GeneratedSnapshot::GeneratedSnapshot() = default;
GeneratedSnapshot::~GeneratedSnapshot() = default;

std::shared_ptr<const GeneratedSnapshot> GeneratedSnapshot::load(RuntimeOptions options, SnapshotLoadLimits limits) {
    if (options.force_source_scan || options.generated_snapshot || limits.maximum_file_bytes == 0 ||
        limits.maximum_payload_bytes == 0 || limits.maximum_file_bytes > limits.maximum_payload_bytes ||
        limits.maximum_payload_bytes > 1024ULL * 1024ULL * 1024ULL)
        throw_usage_error("invalid-snapshot-load", "Snapshot loading requires a bounded generated-index route.");
    options.root = fs::canonical(options.root);
    options.out_root = canonical_snapshot_root(options.out_root.string(), "snapshot output");
    options.force_index = true;
    auto lock = lock_index_for_read(options.out_root);
    const VerifiedSnapshot verified = load_verified_snapshot(options.out_root, limits.maximum_payload_bytes,
                                                             limits.maximum_file_bytes);
    if (verified.generator_version != kGeneratorVersion || verified.profile != options.profile.id ||
        verified.profile_config_hash != profile_config_hash(options.profile))
        throw_index_error("snapshot-route-mismatch", "Snapshot generator or profile does not match the route.", false);
    (void)compact_lexical_manifest(verified.manifest);
    for (const auto& key : expected_generated_relative_files()) {
        if (key != (kGeneratedDir / "manifest.json").generic_string() && !verified.files.contains(key))
            throw_index_error("index-unavailable", "A complete generated index is required for resident loading.");
    }
    std::uintmax_t total = verified.manifest_content.size();
    if (total > limits.maximum_file_bytes || total > limits.maximum_payload_bytes)
        throw CliError("snapshot-memory-budget-exceeded", "resource", "Snapshot exceeds its memory budget.",
                       1, false, "Use a smaller snapshot or review the explicit load budget.");
    for (const auto& [key, entry] : verified.files) {
        (void)key;
        if (entry.bytes > limits.maximum_file_bytes || entry.bytes > limits.maximum_payload_bytes - total)
            throw CliError("snapshot-memory-budget-exceeded", "resource", "Snapshot exceeds its memory budget.",
                           1, false, "Use a smaller snapshot or review the explicit load budget.");
        total += entry.bytes;
    }
    auto result = std::shared_ptr<GeneratedSnapshot>(new GeneratedSnapshot());
    result->options_ = std::move(options);
    result->manifest_ = verified.manifest;
    result->id_ = verified.snapshot_id;
    result->backing_ = std::make_unique<Backing>();
    result->backing_->append(verified.manifest_content);
    for (const auto& [key, entry] : verified.files) {
        if (result->backing_->append(result->options_.out_root / key, entry.bytes) != entry.content_hash)
            throw_index_error("snapshot-integrity-failed", "Loaded snapshot bytes do not match the manifest.");
    }
    result->backing_->seal();
    std::size_t offset = verified.manifest_content.size();
    result->files_.emplace((kGeneratedDir / "manifest.json").generic_string(), result->backing_->view(0, offset));
    for (const auto& [key, entry] : verified.files) {
        const auto bytes = result->backing_->view(offset, static_cast<std::size_t>(entry.bytes));
        result->files_.emplace(key, bytes);
        offset += bytes.size();
    }
    const auto manifest_path = result->options_.out_root / kGeneratedDir / "manifest.json";
    if (bounded_artifact(manifest_path, verified.manifest_content.size(), limits.maximum_file_bytes) != verified.manifest_content)
        throw_index_error("snapshot-integrity-failed", "Snapshot manifest changed while loading.");
    result->payload_bytes_ = total;
    return result;
}

std::string GeneratedSnapshot::relative_key(const fs::path& path) const {
    const auto relative = fs::absolute(path).lexically_normal().lexically_relative(options_.out_root);
    const auto key = relative.generic_string();
    if (!profile_relative_path_valid(key))
        throw_index_error("snapshot-path-outside-view", "Generated read lies outside the loaded snapshot.", false);
    return key;
}

std::string_view GeneratedSnapshot::file(const fs::path& path) const {
    const auto found = files_.find(relative_key(path));
    if (found == files_.end())
        throw_index_error("snapshot-file-not-loaded", "Generated artifact is not part of the loaded snapshot.", false);
    return found->second;
}

bool GeneratedSnapshot::contains(const fs::path& path) const {
    return files_.contains(relative_key(path));
}

GeneratedInput::GeneratedInput(const RuntimeOptions& options, const fs::path& path)
    : std::istream(nullptr), snapshot_(options.generated_snapshot) {
    if (snapshot_) {
        buffer_ = std::make_unique<InputBuffer>(snapshot_->file(path));
        rdbuf(buffer_.get());
    } else {
        file_.open(path, std::ios::binary);
        rdbuf(file_.rdbuf());
        if (!file_) setstate(std::ios_base::failbit);
    }
}

GeneratedInput::~GeneratedInput() = default;

bool generated_file_exists(const RuntimeOptions& options, const fs::path& path) {
    return options.generated_snapshot ? options.generated_snapshot->contains(path) : fs::exists(path);
}

} // namespace shin::context
