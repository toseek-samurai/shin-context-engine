#include "shin/windows/file.h"
#include "internal.h"
#include <windows.h>
#include <sddl.h>
#include <aclapi.h>
#include <bcrypt.h>
#include <psapi.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <vector>

namespace shin::windows {
namespace {
namespace fs = std::filesystem;
using namespace detail;

void validate_path(const fs::path& path) {
    const auto drive = path.root_name().native();
    require(path.is_absolute() && drive.size() == 2 && drive[1] == L':' &&
        ((drive[0] >= L'A' && drive[0] <= L'Z') || (drive[0] >= L'a' && drive[0] <= L'z')),
        "A local absolute drive path is required; UNC/device paths are not admitted");
    require(!path.filename().empty(), "A file path is required");
    for (const auto& component : path.relative_path()) {
        const auto text = component.native();
        require(!text.empty() && text != L"." && text != L".." &&
            text.back() != L'.' && text.back() != L' ' &&
            text.find_first_of(L"<>:\"|?*") == std::wstring::npos &&
            std::none_of(text.begin(), text.end(), [](wchar_t c) { return c < 32; }),
            "Path aliases, streams and invalid components are not admitted");
    }
}

std::wstring extended(const fs::path& path) {
    auto value = path.native();
    std::replace(value.begin(), value.end(), L'/', L'\\');
    return L"\\\\?\\" + value;
}

Handle open_component(const fs::path& path, bool directory, bool inspect_acl = false) {
    Handle handle(CreateFileW(extended(path).c_str(), directory ? FILE_READ_ATTRIBUTES | (inspect_acl ? READ_CONTROL : 0) : GENERIC_READ,
        FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    if (handle.get() == INVALID_HANDLE_VALUE) failed("Cannot pin a local file component");
    FILE_ATTRIBUTE_TAG_INFO tag{};
    if (!GetFileInformationByHandleEx(handle.get(), FileAttributeTagInfo, &tag, sizeof(tag)))
        failed("Cannot inspect a local file component");
    require(GetFileType(handle.get()) == FILE_TYPE_DISK &&
        !(tag.FileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DEVICE)) &&
        ((tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) == directory,
        "File component is not an ordinary file/directory without reparse points");
    return handle;
}

std::int64_t unix_ns(LONGLONG ticks) {
    constexpr std::int64_t epoch = 116444736000000000LL;
    constexpr auto bound = std::numeric_limits<std::int64_t>::max() / 100;
    require(ticks >= epoch - bound && ticks <= epoch + bound, "File timestamp exceeds the supported range");
    return (ticks - epoch) * 100;
}

std::vector<Handle> pin_parents(const fs::path& path, bool inspect_acl = false) {
    validate_path(path);
    auto current = path.root_path();
    require(GetDriveTypeW(current.c_str()) == DRIVE_FIXED, "A fixed local NTFS volume is required");
    std::vector<Handle> parents;
    parents.push_back(open_component(current, true));
    std::array<wchar_t, 32> filesystem{};
    if (!GetVolumeInformationByHandleW(parents.front().get(), nullptr, 0, nullptr, nullptr, nullptr,
                                       filesystem.data(), static_cast<DWORD>(filesystem.size())))
        failed("Cannot identify the local filesystem");
    require(std::wstring_view(filesystem.data()) == L"NTFS", "This adapter is qualified for local NTFS only");
    for (const auto& component : path.relative_path().parent_path()) {
        current /= component;
        parents.push_back(open_component(current, true, inspect_acl));
    }
    return parents;
}


Handle private_temporary_file() {
    const PrivateSecurity security;
    auto attributes = security.attributes();
    for (unsigned attempt = 0; attempt < 8; ++attempt) {
        std::array<unsigned char, 16> random{};
        require(BCryptGenRandom(nullptr, random.data(), static_cast<ULONG>(random.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0,
                "Cannot generate private snapshot name");
        std::wstring name = L"shin-snapshot-";
        for (const auto byte : random) { name += L"0123456789abcdef"[byte >> 4]; name += L"0123456789abcdef"[byte & 15]; }
        const auto path = fs::temp_directory_path() / (name + L".tmp");
        const auto parents = pin_parents(path);
        Handle file(CreateFileW(extended(path).c_str(), GENERIC_READ | GENERIC_WRITE | DELETE, 0, &attributes,
            CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (file.get() != INVALID_HANDLE_VALUE) return file;
        if (GetLastError() != ERROR_FILE_EXISTS && GetLastError() != ERROR_ALREADY_EXISTS)
            failed("Cannot create private snapshot file");
    }
    throw std::runtime_error("Private snapshot name collisions exceeded their bound");
}

FileStamp file_stamp(HANDLE handle) {
    BY_HANDLE_FILE_INFORMATION info{};
    FILE_BASIC_INFO basic{};
    if (!GetFileInformationByHandle(handle, &info) ||
        !GetFileInformationByHandleEx(handle, FileBasicInfo, &basic, sizeof(basic)))
        failed("Cannot read pinned file metadata");
    require(!(info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DEVICE)),
        "Pinned file type changed");
    return {(static_cast<std::uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow,
        info.dwVolumeSerialNumber, (static_cast<std::uint64_t>(info.nFileIndexHigh) << 32) | info.nFileIndexLow,
        unix_ns(basic.LastWriteTime.QuadPart), unix_ns(basic.ChangeTime.QuadPart), info.nNumberOfLinks, info.dwFileAttributes};
}

constexpr std::size_t maximum_image_bytes = 128ULL * 1024ULL * 1024ULL;
template<class T> T image_record(const void* base, std::size_t offset, std::size_t bound) {
    require(offset <= bound && sizeof(T) <= bound - offset, "PE record exceeds image bound");
    const auto* address = static_cast<const unsigned char*>(base) + offset;
    MEMORY_BASIC_INFORMATION memory{};
    require(VirtualQuery(address, &memory, sizeof(memory)) == sizeof(memory) && memory.AllocationBase == base &&
        memory.State == MEM_COMMIT && memory.Type == MEM_IMAGE && !(memory.Protect & (PAGE_NOACCESS | PAGE_GUARD)),
        "PE record is not readable image memory");
    const auto region_offset = reinterpret_cast<std::uintptr_t>(address) - reinterpret_cast<std::uintptr_t>(memory.BaseAddress);
    require(region_offset <= memory.RegionSize && sizeof(T) <= memory.RegionSize - region_offset, "PE record crosses a memory region");
    T value{};
    SIZE_T copied = 0;
    require(ReadProcessMemory(GetCurrentProcess(), address, &value, sizeof(value), &copied) && copied == sizeof(value),
            "Cannot read PE record");
    return value;
}
std::string mapped_image_identity(const void* base, std::size_t bound) {
    require(base && bound <= maximum_image_bytes, "PE image exceeds its bound");
    const auto dos = image_record<IMAGE_DOS_HEADER>(base, 0, bound);
    require(dos.e_magic == IMAGE_DOS_SIGNATURE && dos.e_lfanew > 0, "Invalid DOS image header");
    const auto nt = image_record<IMAGE_NT_HEADERS64>(base, static_cast<std::size_t>(dos.e_lfanew), bound);
    const auto& optional = nt.OptionalHeader;
    require(nt.Signature == IMAGE_NT_SIGNATURE && nt.FileHeader.Machine == IMAGE_FILE_MACHINE_AMD64 &&
        nt.FileHeader.SizeOfOptionalHeader >= sizeof(IMAGE_OPTIONAL_HEADER64) &&
        optional.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC && optional.NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_DEBUG &&
        optional.SizeOfImage > 0 && optional.SizeOfImage <= bound, "Unsupported PE image header");
    bound = optional.SizeOfImage;
    const auto debug = optional.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
    require(debug.VirtualAddress != 0 && debug.Size >= sizeof(IMAGE_DEBUG_DIRECTORY) &&
        debug.Size % sizeof(IMAGE_DEBUG_DIRECTORY) == 0 && debug.Size / sizeof(IMAGE_DEBUG_DIRECTORY) <= 64,
        "PE image requires a bounded debug directory");
    std::string identity;
    for (std::size_t offset = 0; offset < debug.Size; offset += sizeof(IMAGE_DEBUG_DIRECTORY)) {
        const auto entry = image_record<IMAGE_DEBUG_DIRECTORY>(base, static_cast<std::size_t>(debug.VirtualAddress) + offset, bound);
        if (entry.Type != IMAGE_DEBUG_TYPE_CODEVIEW) continue;
        require(identity.empty() && entry.AddressOfRawData && entry.SizeOfData >= 24 &&
            entry.AddressOfRawData <= bound && entry.SizeOfData <= bound - entry.AddressOfRawData,
            "Missing or ambiguous PE CodeView identity");
        const auto record = image_record<std::array<unsigned char, 24>>(base, entry.AddressOfRawData, bound);
        require(std::memcmp(record.data(), "RSDS", 4) == 0 &&
            std::any_of(record.begin() + 4, record.begin() + 20, [](auto byte) { return byte != 0; }),
            "PE image requires a nonzero RSDS build identity");
        DWORD age = 0; std::memcpy(&age, record.data() + 20, sizeof(age));
        require(age != 0, "PE CodeView age is missing");
        identity = "pe-codeview-v1:";
        for (std::size_t i = 4; i < 20; ++i) {
            identity += "0123456789abcdef"[record[i] >> 4]; identity += "0123456789abcdef"[record[i] & 15];
        }
        identity += ':' + std::to_string(age);
    }
    require(!identity.empty(), "PE CodeView identity is missing");
    return identity;
}
}

struct ReadFile::Impl {
    fs::path path;
    std::vector<Handle> parents;
    Handle file{INVALID_HANDLE_VALUE};
    explicit Impl(const fs::path& input) : path(input), parents(pin_parents(path)), file(open_component(path, false)) {}
};

ReadFile::ReadFile(const fs::path& absolute) : impl_(std::make_unique<Impl>(absolute)) {}
ReadFile::~ReadFile() = default;
FileStamp ReadFile::stamp() const { return file_stamp(impl_->file.get()); }
void ReadFile::require_user_owned() const {
    require(stamp().links == 1, "Owned file must be singly linked");
    PrivateSecurity security;
    security.verify_user_directory(impl_->file.get());
}
void ReadFile::require_executable_owner() const {
    require(stamp().links == 1, "Executable must be singly linked");
    PrivateSecurity security;
    security.verify_executable(impl_->file.get());
}
void ReadFile::require_machine_owned() const {
    require(stamp().links == 1, "Service executable must be singly linked");
    PrivateSecurity security;
    security.verify_executable(impl_->file.get(), true);
    // The drive root can allow creation of unrelated children; every named
    // ancestor below it must protect this installation from user replacement.
    // Ordinary source traversal does not require READ_CONTROL on every parent.
    // Reopen only this machine-code inspection with that right, retaining the
    // original non-renamable handles and checking the same objects before ACLs.
    const auto parents = pin_parents(impl_->path, true);
    require(parents.size() == impl_->parents.size(), "Executable ancestor count changed");
    for (std::size_t i = 1; i < parents.size(); ++i) {
        BY_HANDLE_FILE_INFORMATION before{}, current{};
        if (!GetFileInformationByHandle(impl_->parents[i].get(), &before) ||
            !GetFileInformationByHandle(parents[i].get(), &current)) failed("Cannot verify executable ancestor identity");
        if (before.dwVolumeSerialNumber != current.dwVolumeSerialNumber || before.nFileIndexHigh != current.nFileIndexHigh ||
            before.nFileIndexLow != current.nFileIndexLow) throw Changed();
        security.verify_executable(parents[i].get(), true);
    }
}

std::string ReadFile::image_identity() const {
    const auto before = stamp();
    require(before.bytes && before.bytes <= maximum_image_bytes, "PE file exceeds its bound");
    Handle mapping(CreateFileMappingW(impl_->file.get(), nullptr, PAGE_READONLY | SEC_IMAGE_NO_EXECUTE, 0, 0, nullptr));
    if (!mapping.get()) failed("Cannot map PE image without execution");
    struct View { void* data; ~View() { if (data) UnmapViewOfFile(data); } } view{MapViewOfFile(mapping.get(), FILE_MAP_READ, 0, 0, 0)};
    if (!view.data) failed("Cannot read PE image mapping");
    const auto result = mapped_image_identity(view.data, maximum_image_bytes);
    if (stamp() != before) throw Changed();
    return result;
}

namespace {
fs::path module_path() {
    std::vector<wchar_t> value(32768);
    const auto size = GetModuleFileNameW(nullptr, value.data(), static_cast<DWORD>(value.size()));
    require(size && size < value.size(), "Cannot locate running image");
    return fs::path(std::wstring(value.data(), size));
}
struct RunningImage {
    fs::path path = module_path();
    ReadFile pinned{path};
    std::string identity;
    RunningImage() {
        MODULEINFO info{};
        require(K32GetModuleInformation(GetCurrentProcess(), GetModuleHandleW(nullptr), &info, sizeof(info)) != 0,
                "Cannot inspect loaded executable");
        identity = mapped_image_identity(info.lpBaseOfDll, info.SizeOfImage);
        require(identity == pinned.image_identity(), "Named executable differs from the running image");
    }
};
const RunningImage& running_image() {
    static const RunningImage image;
    return image;
}
}
fs::path running_image_path() { return running_image().path; }
std::string running_image_identity() { return running_image().identity; }

std::string ReadFile::read(std::size_t maximum_bytes) {
    std::string result;
    stream(maximum_bytes, [&](std::string_view bytes) { result.append(bytes); });
    return result;
}

void ReadFile::stream(std::size_t maximum_bytes, const std::function<void(std::string_view)>& consume) {
    const auto before = stamp();
    if (before.bytes > maximum_bytes) throw std::length_error("File exceeds its byte budget");
    LARGE_INTEGER zero{};
    if (!SetFilePointerEx(impl_->file.get(), zero, nullptr, FILE_BEGIN)) failed("Cannot position pinned file");
    std::array<char, 65536> buffer{};
    auto remaining = before.bytes;
    while (remaining) {
        DWORD count = 0;
        const auto wanted = static_cast<DWORD>(std::min<std::uint64_t>(buffer.size(), remaining));
        if (!::ReadFile(impl_->file.get(), buffer.data(), wanted, &count, nullptr)) failed("Cannot read pinned file");
        require(count != 0, "Pinned file shrank while reading");
        consume(std::string_view(buffer.data(), count));
        remaining -= count;
    }
    char extra = 0;
    DWORD count = 0;
    if (!::ReadFile(impl_->file.get(), &extra, 1, &count, nullptr)) failed("Cannot verify pinned file boundary");
    require(count == 0 && stamp() == before, "Pinned file changed while reading");
}

struct PrivateMapping::Impl {
    Handle file;
    Handle mapping;
    void* address = nullptr;
    std::size_t size = 0, limit;
    bool sealed = false;
    explicit Impl(std::size_t maximum) : limit(maximum) {
        require(maximum > 0 && maximum <= 1024ULL * 1024ULL * 1024ULL, "Invalid snapshot backing limit");
        file = private_temporary_file();
    }
    ~Impl() { if (address) UnmapViewOfFile(address); }
};

PrivateMapping::PrivateMapping(std::size_t maximum) : impl_(std::make_unique<Impl>(maximum)) {}
PrivateMapping::~PrivateMapping() = default;
void PrivateMapping::append(std::string_view bytes) {
    auto& state = *impl_;
    require(!state.sealed, "Snapshot backing is already sealed");
    if (bytes.size() > state.limit - state.size) throw std::length_error("Snapshot backing exceeds its limit");
    while (!bytes.empty()) {
        DWORD count = 0;
        if (!WriteFile(state.file.get(), bytes.data(), static_cast<DWORD>(std::min<std::size_t>(bytes.size(), 65536)),
                       &count, nullptr)) failed("Cannot write private snapshot backing");
        require(count != 0, "Snapshot backing write made no progress");
        bytes.remove_prefix(count); state.size += count;
    }
}
void PrivateMapping::seal() {
    auto& state = *impl_;
    require(!state.sealed, "Snapshot backing is already sealed");
    if (!FlushFileBuffers(state.file.get())) failed("Cannot flush private snapshot backing");
    if (state.size) {
        state.mapping = Handle(CreateFileMappingW(state.file.get(), nullptr, PAGE_READONLY, 0, 0, nullptr));
        if (!state.mapping.get()) failed("Cannot create private snapshot mapping");
        state.address = MapViewOfFile(state.mapping.get(), FILE_MAP_READ, 0, 0, state.size);
        if (!state.address) failed("Cannot map private snapshot backing");
    }
    state.sealed = true;
    state.file = Handle();
    state.mapping = Handle();
}
std::string_view PrivateMapping::view(std::size_t offset, std::size_t length) const {
    const auto& state = *impl_;
    require(state.sealed && offset <= state.size && length <= state.size - offset, "Invalid snapshot view range");
    return length == 0 ? std::string_view{} : std::string_view(static_cast<const char*>(state.address) + offset, length);
}

struct PrivateCache::Impl {
    fs::path path;
    std::string key;
    PrivateSecurity security;
    std::vector<Handle> parents;
    Handle directory, lock;

    Impl(const fs::path& input, const std::string& name, bool recover_pending) : path(input), key(name), parents(pin_parents(input)) {
        require(key.size() == 16 && key.find_first_not_of("0123456789abcdef") == std::string::npos,
                "Invalid private cache key");
        auto attributes = security.attributes();
        if (!CreateDirectoryW(extended(path).c_str(), &attributes) && GetLastError() != ERROR_ALREADY_EXISTS)
            failed("Cannot create private cache directory");
        directory = Handle(CreateFileW(extended(path).c_str(), FILE_READ_ATTRIBUTES | READ_CONTROL, FILE_SHARE_READ,
            nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (directory.get() == INVALID_HANDLE_VALUE) failed("Cannot pin private cache directory");
        verify(directory.get(), true);
        lock = open(key + ".lock", GENERIC_READ | GENERIC_WRITE, OPEN_ALWAYS);
        if (recover_pending) discard_pending();
    }
    void verify(HANDLE handle, bool is_directory = false) const {
        BY_HANDLE_FILE_INFORMATION info{};
        if (!GetFileInformationByHandle(handle, &info)) failed("Cannot inspect private cache file");
        require(GetFileType(handle) == FILE_TYPE_DISK &&
            !(info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DEVICE)) &&
            ((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) == is_directory &&
            (is_directory || info.nNumberOfLinks == 1), "Private cache type/link count is unsafe");
        security.verify(handle);
    }
    Handle open(const std::string& name, DWORD access, DWORD creation, bool missing_ok = false) const {
        auto attributes = security.attributes();
        Handle handle(CreateFileW(extended(path / name).c_str(), access | READ_CONTROL, 0, &attributes, creation,
            FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr));
        if (handle.get() == INVALID_HANDLE_VALUE) {
            if (missing_ok && GetLastError() == ERROR_FILE_NOT_FOUND) return handle;
            failed("Cannot open private cache slot");
        }
        verify(handle.get());
        return handle;
    }
    void check() const { verify(directory.get(), true); verify(lock.get()); }
    void discard_pending() const {
        auto pending = open(key + ".pending", DELETE | FILE_READ_ATTRIBUTES, OPEN_EXISTING, true);
        if (pending.get() != INVALID_HANDLE_VALUE) {
            FILE_DISPOSITION_INFO disposition{TRUE};
            if (!SetFileInformationByHandle(pending.get(), FileDispositionInfo, &disposition, sizeof(disposition)))
                failed("Cannot discard owned pending cache");
        }
    }
};

PrivateCache::PrivateCache(const fs::path& directory, const std::string& key, bool recover_pending)
    : impl_(std::make_unique<Impl>(directory, key, recover_pending)) {}
PrivateCache::~PrivateCache() = default;
std::optional<std::string> PrivateCache::load(std::size_t maximum_bytes) const {
    const auto& state = *impl_;
    state.check();
    auto file = state.open(state.key + ".sqlite", GENERIC_READ, OPEN_EXISTING, true);
    if (file.get() == INVALID_HANDLE_VALUE) return std::nullopt;
    const auto before = file_stamp(file.get());
    if (before.bytes > maximum_bytes) return std::nullopt;
    std::string result(static_cast<std::size_t>(before.bytes), '\0');
    std::size_t offset = 0;
    while (offset < result.size()) {
        DWORD count = 0;
        if (!::ReadFile(file.get(), result.data() + offset,
            static_cast<DWORD>(std::min<std::size_t>(result.size() - offset, 65536)), &count, nullptr))
            failed("Cannot read private cache");
        require(count != 0, "Private cache shrank while reading");
        offset += count;
    }
    char extra = 0;
    DWORD count = 0;
    if (!::ReadFile(file.get(), &extra, 1, &count, nullptr)) failed("Cannot verify cache boundary");
    require(count == 0 && before == file_stamp(file.get()), "Private cache changed while reading");
    state.verify(file.get());
    state.check();
    return result;
}
void PrivateCache::save(std::string_view bytes) const {
    const auto& state = *impl_;
    state.check();
    // Inspect both fixed slots before any recovery or replacement; never follow links.
    { const auto target = state.open(state.key + ".sqlite", FILE_READ_ATTRIBUTES, OPEN_EXISTING, true); }
    state.discard_pending();
    auto file = state.open(state.key + ".pending", GENERIC_READ | GENERIC_WRITE | DELETE, CREATE_NEW);
    while (!bytes.empty()) {
        DWORD count = 0;
        if (!WriteFile(file.get(), bytes.data(), static_cast<DWORD>(std::min<std::size_t>(bytes.size(), 65536)), &count, nullptr))
            failed("Cannot write private cache");
        require(count != 0, "Private cache write made no progress");
        bytes.remove_prefix(count);
    }
    if (!FlushFileBuffers(file.get())) failed("Cannot flush private cache");
    state.check();
    state.verify(file.get());
    { const auto target = state.open(state.key + ".sqlite", FILE_READ_ATTRIBUTES, OPEN_EXISTING, true); }
    const auto target = extended(state.path / (state.key + ".sqlite"));
    const auto size = sizeof(FILE_RENAME_INFO) + target.size() * sizeof(wchar_t);
    require(size <= std::numeric_limits<DWORD>::max(), "Cache rename path exceeds Windows limit");
    std::vector<unsigned char> storage(size);
    auto* rename = reinterpret_cast<FILE_RENAME_INFO*>(storage.data());
    rename->ReplaceIfExists = TRUE;
    rename->FileNameLength = static_cast<DWORD>(target.size() * sizeof(wchar_t));
    std::memcpy(rename->FileName, target.data(), rename->FileNameLength);
    if (!SetFileInformationByHandle(file.get(), FileRenameInfo, rename, static_cast<DWORD>(size)))
        failed("Cannot publish complete private cache");
    state.verify(file.get());
    state.check();
}

namespace {
std::string directory_id(HANDLE handle) {
    FILE_ID_INFO file{};
    FILE_BASIC_INFO basic{};
    if (!GetFileInformationByHandleEx(handle, FileIdInfo, &file, sizeof(file)) ||
        !GetFileInformationByHandleEx(handle, FileBasicInfo, &basic, sizeof(basic)))
        failed("Cannot identify persistent directory");
    std::vector<wchar_t> path(32768);
    const auto size = GetFinalPathNameByHandleW(handle, path.data(), static_cast<DWORD>(path.size()), VOLUME_NAME_GUID);
    if (!size || size >= path.size()) failed("Cannot resolve persistent volume GUID");
    const std::wstring_view value(path.data(), size);
    require(value.starts_with(L"\\\\?\\Volume{"), "A persistent volume GUID is required");
    const auto end = value.find(L'}');
    require(end == 47 && basic.CreationTime.QuadPart > 0, "Invalid volume or directory birth identity");
    std::string result = "win-volume-v1:";
    for (std::size_t i = 11; i < end; ++i) {
        auto c = value[i];
        if (c >= L'A' && c <= L'F') c += L'a' - L'A';
        require((c >= L'a' && c <= L'f') || (c >= L'0' && c <= L'9') || c == L'-', "Invalid volume GUID");
        result += static_cast<char>(c);
    }
    result += ':';
    for (const auto byte : file.FileId.Identifier) {
        result += "0123456789abcdef"[byte >> 4]; result += "0123456789abcdef"[byte & 15];
    }
    return result + ':' + std::to_string(basic.CreationTime.QuadPart);
}
fs::path leaf_name(const std::string& name) {
    require(!name.empty() && name.size() <= 1020 && name != "." && name != ".." &&
        name.back() != '.' && name.back() != ' ' &&
        name.find_first_of("/\\<>:\"|?*") == std::string::npos &&
        std::none_of(name.begin(), name.end(), [](unsigned char c) { return c < 32 || c == 127; }),
        "Private file name must be one bounded UTF-8 component");
    const auto size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name.data(), static_cast<int>(name.size()), nullptr, 0);
    require(size > 0 && size <= 255, "Invalid UTF-8 file name or component length");
    std::wstring wide(static_cast<std::size_t>(size), L'\0');
    require(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name.data(), static_cast<int>(name.size()), wide.data(), size) == size,
            "Cannot decode file name");
    return fs::path(wide);
}
void private_regular(HANDLE file, const PrivateSecurity& security) {
    const auto links = file_stamp(file).links;
    if (!links) throw Changed();
    require(links == 1, "Private file must be singly linked");
    security.verify(file);
}
void write_flushed(HANDLE file, std::string_view bytes) {
    while (!bytes.empty()) {
        DWORD count = 0;
        if (!WriteFile(file, bytes.data(), static_cast<DWORD>(std::min<std::size_t>(bytes.size(), 65536)), &count, nullptr))
            failed("Cannot write private file");
        require(count != 0, "Private write made no progress");
        bytes.remove_prefix(count);
    }
    if (!FlushFileBuffers(file)) failed("Cannot synchronize private file");
}
std::string read_handle(HANDLE file, std::size_t limit) {
    const auto before = file_stamp(file);
    if (before.bytes > limit) throw std::length_error("Private file exceeds its byte limit");
    std::string result(static_cast<std::size_t>(before.bytes), '\0');
    std::size_t offset = 0;
    while (offset < result.size()) {
        DWORD count = 0;
        if (!::ReadFile(file, result.data() + offset,
            static_cast<DWORD>(std::min<std::size_t>(result.size() - offset, 65536)), &count, nullptr)) failed("Cannot read private file");
        if (!count) throw Changed();
        offset += count;
    }
    char extra = 0; DWORD count = 0;
    if (!::ReadFile(file, &extra, 1, &count, nullptr)) failed("Cannot verify private file boundary");
    if (count || before != file_stamp(file)) throw Changed();
    return result;
}
void publish_handle(HANDLE file, const fs::path& target_path) {
    const auto target = extended(target_path);
    const auto size = sizeof(FILE_RENAME_INFO) + target.size() * sizeof(wchar_t);
    require(size <= std::numeric_limits<DWORD>::max(), "State path exceeds Windows bound");
    std::vector<unsigned char> storage(size);
    auto* rename = reinterpret_cast<FILE_RENAME_INFO*>(storage.data());
    rename->FileNameLength = static_cast<DWORD>(target.size() * sizeof(wchar_t));
    std::memcpy(rename->FileName, target.data(), rename->FileNameLength);
    // Readers retain the old complete object and recheck the named identity.
    rename->Flags = FILE_RENAME_FLAG_REPLACE_IF_EXISTS | FILE_RENAME_FLAG_POSIX_SEMANTICS;
    if (!SetFileInformationByHandle(file, FileRenameInfoEx, rename, static_cast<DWORD>(size))) failed("Cannot commit private state");
}
struct PendingCleanup {
    HANDLE file;
    bool committed = false;
    ~PendingCleanup() {
        if (!committed) {
            FILE_DISPOSITION_INFO disposition{TRUE};
            SetFileInformationByHandle(file, FileDispositionInfo, &disposition, sizeof(disposition));
        }
    }
};
}

struct Directory::Impl {
    fs::path path;
    PrivateSecurity security;
    std::vector<Handle> parents;
    Handle directory;
    Directory::Access policy;
    std::string id;
    Impl(const fs::path& input, Directory::Access requested) : path(input), parents(pin_parents(input)), policy(requested) {
        directory = Handle(CreateFileW(extended(path).c_str(), FILE_READ_ATTRIBUTES | READ_CONTROL,
            FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (directory.get() == INVALID_HANDLE_VALUE) failed("Cannot pin state directory");
        FILE_ATTRIBUTE_TAG_INFO tag{};
        if (!GetFileInformationByHandleEx(directory.get(), FileAttributeTagInfo, &tag, sizeof(tag))) failed("Cannot inspect directory");
        require((tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT),
                "Unsafe directory type");
        id = directory_id(directory.get());
        check();
    }
    void check() const {
        if (directory_id(directory.get()) != id) throw Changed();
        if (policy == Directory::Access::private_owner) security.verify(directory.get());
        if (policy == Directory::Access::user_owned) security.verify_user_directory(directory.get());
    }
    void writable() const {
        require(policy != Directory::Access::read_only, "Read-only directory cannot publish files");
        check();
    }
    Handle open(const std::string& name, DWORD access, DWORD mode, bool missing = false, DWORD share = FILE_SHARE_READ | FILE_SHARE_DELETE) const {
        const auto leaf = leaf_name(name); check();
        auto attributes = security.attributes();
        Handle file(CreateFileW(extended(path / leaf).c_str(), access | READ_CONTROL, share, &attributes, mode,
            FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr));
        if (file.get() == INVALID_HANDLE_VALUE) {
            if (missing && GetLastError() == ERROR_FILE_NOT_FOUND) return file;
            failed("Cannot open state file");
        }
        (void)file_stamp(file.get());
        return file;
    }
};
struct WriterLock::Impl {
    PrivateSecurity security;
    std::vector<Handle> parents;
    Handle file;
    FileStamp before;
    bool locked = false;
    ~Impl() {
        if (locked) { OVERLAPPED position{}; UnlockFileEx(file.get(), 0, 1, 0, &position); }
    }
};
WriterLock::WriterLock(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
WriterLock::~WriterLock() = default;
void WriterLock::check() const {
    private_regular(impl_->file.get(), impl_->security);
    if (file_stamp(impl_->file.get()) != impl_->before) throw Changed();
}
Directory::Directory(const fs::path& path, Access access) : impl_(std::make_unique<Impl>(path, access)) {}
Directory::~Directory() = default;
void Directory::ensure_private(const fs::path& path, bool exclusive) {
    const auto parents = pin_parents(path);
    PrivateSecurity security;
    auto attributes = security.attributes();
    if (!CreateDirectoryW(extended(path).c_str(), &attributes) && (exclusive || GetLastError() != ERROR_ALREADY_EXISTS))
        failed("Cannot create private directory");
    Directory checked(path, Access::private_owner);
}
void Directory::ensure_user_owned(const fs::path& path) {
    const auto parents = pin_parents(path);
    PrivateSecurity security;
    auto attributes = security.attributes();
    if (!CreateDirectoryW(extended(path).c_str(), &attributes) && GetLastError() != ERROR_ALREADY_EXISTS)
        failed("Cannot create owned directory");
    Directory checked(path, Access::user_owned);
}
void Directory::check() const { impl_->check(); }
std::string Directory::identity() const { check(); return impl_->id; }
std::optional<std::string> Directory::read(const std::string& name, std::size_t limit, bool private_only) const {
    auto file = impl_->open(name, GENERIC_READ, OPEN_EXISTING, true);
    if (file.get() == INVALID_HANDLE_VALUE) return std::nullopt;
    if (private_only) private_regular(file.get(), impl_->security);
    const auto before = file_stamp(file.get());
    auto result = read_handle(file.get(), limit);
    auto named = impl_->open(name, FILE_READ_ATTRIBUTES, OPEN_EXISTING, true);
    if (named.get() == INVALID_HANDLE_VALUE || before != file_stamp(named.get()) || before != file_stamp(file.get())) throw Changed();
    if (private_only) private_regular(file.get(), impl_->security);
    check(); return result;
}
bool Directory::create(const std::string& name, std::string_view content) const {
    impl_->writable();
    Handle file;
    try { file = impl_->open(name, GENERIC_READ | GENERIC_WRITE, CREATE_NEW, false, FILE_SHARE_READ); }
    catch (const std::system_error& error) {
        if (error.code().value() != ERROR_FILE_EXISTS && error.code().value() != ERROR_ALREADY_EXISTS) throw;
        if (read(name, content.size()) != content) throw Changed();
        return false;
    }
    private_regular(file.get(), impl_->security);
    write_flushed(file.get(), content);
    private_regular(file.get(), impl_->security); check();
    return true;
}
std::optional<FileStamp> Directory::metadata(const std::string& name, std::size_t limit) const {
    auto file = impl_->open(name, FILE_READ_ATTRIBUTES, OPEN_EXISTING, true, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE);
    if (file.get() == INVALID_HANDLE_VALUE) return {};
    private_regular(file.get(), impl_->security);
    const auto stamp = file_stamp(file.get());
    require(stamp.bytes <= limit, "Private database exceeds its byte limit");
    check(); return stamp;
}
bool Directory::create_empty_file(const std::string& name) const {
    impl_->writable();
    try {
        auto file = impl_->open(name, GENERIC_READ | GENERIC_WRITE, CREATE_NEW, false, FILE_SHARE_READ | FILE_SHARE_WRITE);
        private_regular(file.get(), impl_->security);
        require(FlushFileBuffers(file.get()) != 0, "Cannot flush private database file");
        return true;
    } catch (const std::system_error& error) {
        if (error.code().value() != ERROR_FILE_EXISTS && error.code().value() != ERROR_ALREADY_EXISTS) throw;
        return false;
    }
}
void Directory::publish(const std::string& name, const std::string& pending, std::string_view content) const {
    leaf_name(name); leaf_name(pending); require(name != pending, "Pending must be a separate file");
    impl_->writable();
    { auto target = impl_->open(name, FILE_READ_ATTRIBUTES, OPEN_EXISTING, true);
      if (target.get() != INVALID_HANDLE_VALUE) private_regular(target.get(), impl_->security); }
    auto file = impl_->open(pending, GENERIC_READ | GENERIC_WRITE | DELETE, CREATE_NEW, false, FILE_SHARE_READ);
    private_regular(file.get(), impl_->security);
    write_flushed(file.get(), content);
    check(); private_regular(file.get(), impl_->security);
    { auto target = impl_->open(name, FILE_READ_ATTRIBUTES, OPEN_EXISTING, true);
      if (target.get() != INVALID_HANDLE_VALUE) private_regular(target.get(), impl_->security); }
    publish_handle(file.get(), impl_->path / leaf_name(name));
    if (!FlushFileBuffers(file.get())) failed("Cannot synchronize committed state");
    private_regular(file.get(), impl_->security); check();
}
void Directory::replace(const std::string& name, std::string_view expected, std::string_view content, std::size_t limit) const {
    if (content.size() > limit) throw std::length_error("Replacement exceeds its byte limit");
    impl_->writable();
    auto original = impl_->open(name, GENERIC_READ, OPEN_EXISTING);
    private_regular(original.get(), impl_->security);
    const auto before = file_stamp(original.get());
    const auto bytes = read_handle(original.get(), limit);
    const auto unchanged = [&] {
        check();
        auto named = impl_->open(name, FILE_READ_ATTRIBUTES, OPEN_EXISTING);
        private_regular(named.get(), impl_->security);
        if (file_stamp(named.get()) != before || file_stamp(original.get()) != before) throw Changed();
    };
    if (bytes == content) { unchanged(); return; }
    if (bytes != expected) throw Changed();
    std::array<unsigned char, 16> random{};
    require(BCryptGenRandom(nullptr, random.data(), static_cast<ULONG>(random.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0,
            "Cannot generate archive staging name");
    std::string pending = ".shin-agent-";
    for (const auto byte : random) { pending += "0123456789abcdef"[byte >> 4]; pending += "0123456789abcdef"[byte & 15]; }
    pending += ".tmp";
    auto file = impl_->open(pending, GENERIC_READ | GENERIC_WRITE | DELETE, CREATE_NEW, false, FILE_SHARE_READ);
    PendingCleanup cleanup{file.get()};
    private_regular(file.get(), impl_->security);
    write_flushed(file.get(), content);
    check(); private_regular(file.get(), impl_->security);
    unchanged();
    publish_handle(file.get(), impl_->path / leaf_name(name));
    cleanup.committed = true;
    if (!FlushFileBuffers(file.get())) failed("Cannot synchronize replaced archive");
    private_regular(file.get(), impl_->security); check();
}
bool Directory::remove(const std::string& name, std::size_t limit, const std::optional<std::string>& expected) const {
    impl_->writable();
    auto file = impl_->open(name, GENERIC_READ | DELETE, OPEN_EXISTING, true, FILE_SHARE_READ);
    if (file.get() == INVALID_HANDLE_VALUE) return false;
    private_regular(file.get(), impl_->security);
    const auto bytes = read_handle(file.get(), limit);
    if (expected && bytes != *expected) throw Changed();
    check(); private_regular(file.get(), impl_->security);
    FILE_DISPOSITION_INFO disposition{TRUE};
    if (!SetFileInformationByHandle(file.get(), FileDispositionInfo, &disposition, sizeof(disposition))) failed("Cannot remove private state file");
    return true;
}
std::unique_ptr<WriterLock> Directory::writer(const std::string& name, bool create) const {
    impl_->security.verify(impl_->directory.get());
    return lock(name, true, create, false);
}
std::unique_ptr<WriterLock> Directory::lock(const std::string& name, bool exclusive, bool create, bool wait) const {
    impl_->writable();
    require(!create || exclusive, "Readers must not create locks");
    auto held = std::make_unique<WriterLock::Impl>();
    held->parents = pin_parents(impl_->path / leaf_name(name));
    held->file = impl_->open(name, exclusive ? GENERIC_READ | GENERIC_WRITE : GENERIC_READ, create ? OPEN_ALWAYS : OPEN_EXISTING,
                             false, FILE_SHARE_READ | FILE_SHARE_WRITE);
    private_regular(held->file.get(), held->security);
    OVERLAPPED position{};
    const DWORD flags = (exclusive ? LOCKFILE_EXCLUSIVE_LOCK : 0) | (wait ? 0 : LOCKFILE_FAIL_IMMEDIATELY);
    if (!LockFileEx(held->file.get(), flags, 0, 1, 0, &position))
        failed("Cannot acquire state writer lock");
    held->locked = true;
    held->before = file_stamp(held->file.get());
    return std::unique_ptr<WriterLock>(new WriterLock(std::move(held)));
}
bool Directory::writer_present(const std::string& name) const {
    auto probe = impl_->open(name, FILE_READ_ATTRIBUTES, OPEN_EXISTING, true, FILE_SHARE_READ | FILE_SHARE_WRITE);
    if (probe.get() == INVALID_HANDLE_VALUE) return false;
    private_regular(probe.get(), impl_->security);
    try { const auto held = writer(name, false); return false; }
    catch (const std::system_error& error) {
        if (error.code().value() != ERROR_LOCK_VIOLATION) throw;
        return true;
    }
}
}
