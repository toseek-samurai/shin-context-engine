#include "shin/observer/updater_bundle.h"
#include "shin/observer/observer.h"
#include <algorithm>
#include <set>
#include <sys/stat.h>
#include <unistd.h>
namespace shin::observer {
namespace {
[[noreturn]] void invalid() { throw Error("update-bootstrap-invalid", "The updater package failed its stable protocol, inventory or integrity checks."); }
void require(bool ok) { if (!ok) invalid(); }
bool safe_path(const std::string& name) {
    const fs::path path(name);
    return !name.empty() && name.size() <= 512 && name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._/-") == std::string::npos &&
        !path.is_absolute() && path.lexically_normal() == path &&
        std::none_of(path.begin(), path.end(), [](const auto& part) { return part == ".." || part == "."; });
}
void regular(const fs::path& path) {
    struct stat value{};
    require(::lstat(path.c_str(), &value) == 0 && S_ISREG(value.st_mode) && value.st_nlink == 1 &&
        value.st_uid == ::geteuid() && !(value.st_mode & 07022));
}
std::uint32_t number(const std::string& bytes, std::size_t at, unsigned count) {
    require(at <= bytes.size() && count <= bytes.size() - at);
    std::uint32_t value = 0;
    for (unsigned i = 0; i < count; ++i) value |= static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at + i])) << (i * 8);
    return value;
}
}
void verify_updater_archive(const std::string& bytes) {
    require(bytes.size() >= 22 && bytes.size() <= updater_archive_limit);
    // v1 deliberately uses ordinary ZIP, one disk, no comment/ZIP64/encryption.
    const auto end = bytes.size() - 22;
    require(number(bytes,end,4) == 0x06054b50 && number(bytes,end+4,4) == 0 && number(bytes,end+20,2) == 0);
    const auto count = number(bytes,end+10,2), directory_size = number(bytes,end+12,4), start = number(bytes,end+16,4);
    require(count && count <= 256 && number(bytes,end+8,2) == count && start <= end && directory_size == end - start);
    std::size_t at = start, total = 0;
    std::set<std::string> paths;
    for (unsigned i = 0; i < count; ++i) {
        require(number(bytes,at,4) == 0x02014b50);
        const auto flags = number(bytes,at+8,2), method = number(bytes,at+10,2);
        const auto compressed = number(bytes,at+20,4), size = number(bytes,at+24,4);
        const auto length = number(bytes,at+28,2), extra = number(bytes,at+30,2), comment = number(bytes,at+32,2);
        const auto attributes = number(bytes,at+38,4), local = number(bytes,at+42,4);
        require(length && length <= 512 && at + 46 + length <= end && extra == 0 && comment == 0 && flags == 0 &&
            (method == 0 || method == 8) && size <= 64U*1024*1024 && number(bytes,at+34,2) == 0);
        auto name = bytes.substr(at+46,length);
        const bool directory = name.back() == '/'; if (directory) name.pop_back();
        require(safe_path(name) && (name == updater_bundle_name || name.starts_with(std::string(updater_bundle_name)+"/")) && paths.insert(name).second);
        const auto type = (attributes >> 16) & S_IFMT;
        require(type == 0 || (directory ? type == S_IFDIR : type == S_IFREG));
        total += size; require(total <= 128U*1024*1024);
        require(local < start && number(bytes,local,4) == 0x04034b50 && number(bytes,local+6,2) == flags && number(bytes,local+8,2) == method);
        const auto local_length = number(bytes,local+26,2), local_extra = number(bytes,local+28,2);
        require(local_length == length && local_extra == 0 && bytes.substr(local+30,local_length) == bytes.substr(at+46,length) &&
            static_cast<std::uint64_t>(local)+30+local_length+local_extra+compressed <= start);
        at += 46 + length + extra + comment;
    }
    require(at == end && paths.contains(std::string(updater_bundle_name)+"/shin-updater.json") &&
        paths.contains(std::string(updater_bundle_name)+"/bin/shin-updater"));
}
Json verify_updater_bundle(const fs::path& root, bool inside_kit) try {
    require(fs::canonical(root) == root); (void)directory_identity(root);
    regular(root/"shin-updater.json");
    const auto manifest = parse_json(read_text(root/"shin-updater.json",64U*1024,false));
    require(manifest.is_object() && manifest.size() == 4 && manifest.value("schema_version","") == "shin.updater.v1" &&
        manifest.value("protocol",0) == 1 && manifest.contains("version") && manifest["version"].is_string() &&
        manifest["version"].get<std::string>().size() <= 64 && manifest["files"].is_array() && !manifest["files"].empty() && manifest["files"].size() <= 255);
    std::set<fs::path> files{"shin-updater.json"}, dirs;
    std::size_t total = 0;
    for (const auto& row : manifest["files"]) {
        require(row.is_object() && row.size() == 4 && row["path"].is_string() && row["sha256"].is_string() && row["size"].is_number_unsigned() && row["executable"].is_boolean());
        const auto name = row["path"].get<std::string>(), hash = row["sha256"].get<std::string>();
        require(safe_path(name) && files.insert(name).second && hash.size() == 64 && hash.find_first_not_of("0123456789abcdef") == std::string::npos);
        const auto size = row["size"].get<std::uint64_t>(); require(size <= 64U*1024*1024);
        total += size; require(total <= 128U*1024*1024);
        const fs::path path = name;
        for (auto parent = path.parent_path(); !parent.empty(); parent = parent.parent_path()) {
            dirs.insert(parent); (void)directory_identity(root/parent);
        }
        regular(root/path);
        const auto bytes = read_text(root/path,64U*1024*1024,false);
        require(bytes.size() == size && digest(bytes) == hash && (!row["executable"].get<bool>() || ::access((root/path).c_str(),X_OK) == 0));
    }
    require(files.contains("bin/shin-updater"));
    if (!inside_kit) {
        std::size_t count = 0;
        for (const auto& entry : fs::recursive_directory_iterator(root)) {
            require(++count <= files.size()+dirs.size());
            const auto relative = entry.path().lexically_relative(root);
            require(entry.is_directory() ? dirs.contains(relative) : entry.is_regular_file() && files.contains(relative));
            require(!entry.is_symlink());
        }
    }
    return manifest;
} catch(const Json::exception&) {
    invalid();
}
} // namespace shin::observer
