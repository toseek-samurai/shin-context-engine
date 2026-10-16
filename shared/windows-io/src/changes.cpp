#include "shin/windows/changes.h"
#include "shin/windows/file.h"
#include "internal.h"
#include <array>
#include <atomic>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>

namespace shin::windows {
namespace {
namespace fs = std::filesystem;
using namespace detail;
constexpr std::size_t buffer_bytes = 16 * 1024;
bool below(const fs::path& path, const fs::path& root) {
    auto a = path.begin(), b = root.begin();
    for (; b != root.end(); ++a, ++b) {
        if (a == path.end() || CompareStringOrdinal(a->c_str(), -1, b->c_str(), -1, TRUE) != CSTR_EQUAL) return false;
    }
    return true;
}
struct Watch {
    fs::path path;
    std::wstring opened_name;
    bool recursive = false, pending = false;
    Handle directory;
    Handle event{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    OVERLAPPED operation{};
    alignas(DWORD) std::array<unsigned char, buffer_bytes> bytes{};
    Watch(const fs::path& input, bool subtree) : path(input), recursive(subtree) {
        path.make_preferred();
        // Pin only while opening. A watcher must observe root moves, not prevent them.
        std::unique_ptr<Directory> pin;
        if (path != path.root_path()) pin = std::make_unique<Directory>(path);
        const auto full = L"\\\\?\\" + path.native();
        directory = Handle(CreateFileW(full.c_str(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_OVERLAPPED, nullptr));
        if (!event.get() || directory.get() == INVALID_HANDLE_VALUE) failed("Cannot open native change monitor");
        FILE_ATTRIBUTE_TAG_INFO attributes{};
        require(GetFileInformationByHandleEx(directory.get(), FileAttributeTagInfo, &attributes, sizeof(attributes)) &&
            (attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT),
            "Change monitor root is not an ordinary directory");
        operation.hEvent = event.get();
        opened_name = current_name();
    }
    std::wstring current_name() const {
        std::array<wchar_t, 8192> name{};
        const auto size = GetFinalPathNameByHandleW(directory.get(), name.data(), static_cast<DWORD>(name.size()),
            FILE_NAME_NORMALIZED | VOLUME_NAME_GUID);
        require(size && size < name.size(), "Cannot resolve watched directory identity");
        return {name.data(), size};
    }
    void verify() const {
        FILE_STANDARD_INFO standard{};
        FILE_ATTRIBUTE_TAG_INFO attributes{};
        require(GetFileInformationByHandleEx(directory.get(), FileStandardInfo, &standard, sizeof(standard)) &&
            GetFileInformationByHandleEx(directory.get(), FileAttributeTagInfo, &attributes, sizeof(attributes)) &&
            !standard.DeletePending && (attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
            !(attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) && current_name() == opened_name,
            "Watched directory moved, disappeared or changed type");
    }
    void arm() {
        ResetEvent(event.get());
        operation = {}; operation.hEvent = event.get();
        const DWORD filter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
            FILE_NOTIFY_CHANGE_ATTRIBUTES | FILE_NOTIFY_CHANGE_SECURITY |
            (recursive ? FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE : 0);
        if (!ReadDirectoryChangesExW(directory.get(), bytes.data(), static_cast<DWORD>(bytes.size()), recursive,
            filter, nullptr, &operation, nullptr, ReadDirectoryNotifyExtendedInformation)) failed("Cannot arm native change monitor");
        pending = true;
    }
    ~Watch() {
        if (pending) {
            CancelIoEx(directory.get(), &operation);
            DWORD ignored = 0; GetOverlappedResult(directory.get(), &operation, &ignored, TRUE);
        }
    }
};
}
struct DirectoryChanges::Impl {
    std::vector<fs::path> roots;
    std::vector<std::unique_ptr<Watch>> watches;
    std::function<void(const fs::path&, bool)> changed;
    std::function<void()> lost;
    Handle wake{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    std::atomic<bool> stopping{false}, damaged{false};
    std::mutex delivery;
    std::thread worker;
    inline static thread_local Impl* delivering = nullptr;
    Impl(std::function<void(const fs::path&, bool)> callback, std::function<void()> failure)
        : changed(std::move(callback)), lost(std::move(failure)) {
        require(changed && lost && wake.get(), "Native monitor requires callbacks and a wake event");
    }
    void fail() noexcept {
        if (!damaged.exchange(true)) { try { lost(); } catch (...) {} }
        SetEvent(wake.get());
    }
    void verify_roots() {
        // Inspect existing delete-shared handles. Reopening pinned Directory
        // objects here would intermittently prevent the user's own renames.
        for (const auto& watch : watches) watch->verify();
    }
    void deliver(const Watch& watch, DWORD size) {
        constexpr auto header_size = offsetof(FILE_NOTIFY_EXTENDED_INFORMATION, FileName);
        require(size && size <= watch.bytes.size(), "Native change event buffer lost data");
        std::size_t offset = 0;
        for (;;) {
            require(offset <= size && header_size <= size - offset, "Truncated native change record");
            FILE_NOTIFY_EXTENDED_INFORMATION entry{};
            std::memcpy(&entry, watch.bytes.data() + offset, header_size);
            require(entry.FileNameLength && entry.FileNameLength <= 8192 && entry.FileNameLength % sizeof(wchar_t) == 0 &&
                entry.FileNameLength <= size - offset - header_size && entry.Action >= FILE_ACTION_ADDED && entry.Action <= FILE_ACTION_RENAMED_NEW_NAME,
                "Invalid native change record");
            std::wstring name(entry.FileNameLength / sizeof(wchar_t), L'\0');
            std::memcpy(name.data(), watch.bytes.data() + offset + header_size, entry.FileNameLength);
            const fs::path relative(name);
            require(!relative.has_root_directory() && !relative.has_root_name() && name.find(L'\0') == std::wstring::npos &&
                    relative.lexically_normal() == relative, "Unsafe native event path");
            for (const auto& part : relative) require(part != L".." && part != L".", "Native event escaped its watch");
            const auto path = watch.path / relative;
            const bool directory = (entry.FileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0;
            for (const auto& root : roots) {
                if (below(root, path)) {
                    // Root/ancestor rename or deletion cannot be filtered away.
                    if (entry.Action != FILE_ACTION_MODIFIED) throw Changed();
                    verify_roots(); changed(root, true); break;
                }
                if (watch.recursive && below(path, root)) { changed(path, directory); break; }
            }
            if (!entry.NextEntryOffset) break;
            require(entry.NextEntryOffset % sizeof(DWORD) == 0 && entry.NextEntryOffset >= header_size + entry.FileNameLength &&
                entry.NextEntryOffset < size - offset, "Invalid native event link");
            offset += entry.NextEntryOffset;
        }
    }
    void drain() noexcept {
        struct Scope {
            Impl*& slot;
            Impl* previous;
            ~Scope() { slot = previous; }
        } scope{delivering, delivering};
        delivering = this;
        try {
            if (damaged.load() || stopping.load()) return;
            verify_roots();
            for (auto& watch : watches) {
                if (WaitForSingleObject(watch->event.get(), 0) != WAIT_OBJECT_0) continue;
                DWORD count = 0;
                if (!GetOverlappedResult(watch->directory.get(), &watch->operation, &count, FALSE)) failed("Native change delivery failed");
                watch->pending = false;
                deliver(*watch, count);
                watch->arm();
            }
        } catch (...) { fail(); }
    }
    void start(const std::vector<fs::path>& input) {
        require(!input.empty(), "Native monitor requires roots");
        std::map<fs::path, bool> selected;
        for (const auto& path : input) {
            (void)Directory(path).identity();
            roots.push_back(path); selected[path] = true;
            for (auto parent = path.parent_path(); !parent.empty(); parent = parent.parent_path()) {
                selected.try_emplace(parent, false);
                require(selected.size() < MAXIMUM_WAIT_OBJECTS, "Native monitor root/ancestor budget exceeded");
                if (parent == parent.root_path()) break;
            }
        }
        require(selected.size() < MAXIMUM_WAIT_OBJECTS, "Native monitor root/ancestor budget exceeded");
        for (const auto& [path, recursive] : selected) watches.push_back(std::make_unique<Watch>(path, recursive));
        for (auto& watch : watches) watch->arm();
        verify_roots();
        worker = std::thread([this] {
            std::vector<HANDLE> handles{wake.get()};
            for (const auto& watch : watches) handles.push_back(watch->event.get());
            while (!stopping.load() && !damaged.load()) {
                const auto result = WaitForMultipleObjects(static_cast<DWORD>(handles.size()), handles.data(), FALSE, INFINITE);
                if (result == WAIT_OBJECT_0) return;
                if (result == WAIT_FAILED || result >= WAIT_OBJECT_0 + handles.size()) { fail(); return; }
                std::lock_guard<std::mutex> lock(delivery); drain();
            }
        });
    }
    void flush() {
        require(delivering != this, "Cannot reenter native change delivery");
        std::lock_guard<std::mutex> lock(delivery); drain();
    }
    ~Impl() {
        stopping.store(true); SetEvent(wake.get());
        if (worker.joinable()) worker.join();
        watches.clear();
    }
};
DirectoryChanges::DirectoryChanges(const std::vector<fs::path>& roots,
    std::function<void(const fs::path&, bool)> changed, std::function<void()> lost)
    : impl_(std::make_unique<Impl>(std::move(changed), std::move(lost))) { impl_->start(roots); }
DirectoryChanges::~DirectoryChanges() = default;
void DirectoryChanges::flush() { impl_->flush(); }
}
