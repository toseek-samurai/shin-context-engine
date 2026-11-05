#include "shin/observer/local_mcp_bridge.h"
#include "shin/observer/local_service.h"
#include "shin/observer/local_installation.h"
#include "shin/observer/local_package.h"
#include <mutex>
#include <set>
#include <algorithm>
#include "mcp_adapter_contract.h"
#ifdef _WIN32
#include "shin/windows/file.h"
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace shin::observer {
namespace {
constexpr auto runtime_path = "share/shin-context/mcp-runtime.json";
constexpr auto protocol = "shin.mcp-service.v1";
constexpr std::size_t max_runtime_file = 128U * 1024 * 1024;

void require_runtime(bool valid) {
    if (!valid) throw Error("mcp-runtime-invalid", "The installed MCP runtime could not be verified. Preserve the installation and review its update state.");
}
Json stamp(const fs::path& path) {
#ifdef _WIN32
    shin::windows::ReadFile file(path);
    file.require_executable_owner();
    const auto value = file.stamp();
    require_runtime(value.links == 1);
    return Json::array({value.volume,value.file,value.bytes,value.modified_ns,value.changed_ns,value.attributes});
#else
    struct stat value{};
    require_runtime(fs::canonical(path) == path && ::lstat(path.c_str(), &value) == 0 &&
        S_ISREG(value.st_mode) && value.st_nlink == 1 && !(value.st_mode & 07022) &&
        (value.st_uid == ::geteuid() || value.st_uid == 0));
#ifdef __APPLE__
    const auto modified = value.st_mtimespec, changed = value.st_ctimespec;
#else
    const auto modified = value.st_mtim, changed = value.st_ctim;
#endif
    return Json::array({value.st_dev,value.st_ino,value.st_size,modified.tv_sec,modified.tv_nsec,
        changed.tv_sec,changed.tv_nsec,value.st_mode,value.st_uid});
#endif
}
std::string runtime_file(const fs::path& path, std::size_t limit) {
    (void)stamp(path);
    return read_text(path,limit,false);
}
struct Runtime {
    Json manifest, binding;
    std::string fingerprint;
};
Runtime inspect(const fs::path& prefix) {
    const auto text = runtime_file(prefix / runtime_path,kMaximumStateBytes);
    Runtime result{parse_json(text),{{"protocol",protocol},{"runtime_sha256",digest(text)}}, {}};
    const auto& manifest = result.manifest;
    require_runtime(manifest.is_object() && manifest.size() == 5 &&
        manifest.contains("schema_version") && manifest.at("schema_version").is_string() &&
        manifest.contains("adapter_contract_sha256") && manifest.at("adapter_contract_sha256").is_string() &&
        manifest.contains("observer_version") && manifest.at("observer_version").is_string() &&
        manifest.contains("core_version") && manifest.at("core_version").is_string() &&
        !validation_identifier(manifest.at("observer_version")).empty() &&
        !validation_identifier(manifest.at("core_version")).empty() &&
        manifest.contains("files") && manifest.at("files").is_array() &&
        !manifest.at("files").empty() && manifest.at("files").size() <= 128);
    if (manifest.value("schema_version","") != protocol || manifest.at("adapter_contract_sha256") != SHIN_MCP_ADAPTER_CONTRACT)
        throw Error("mcp-protocol-changed", "This update changed the MCP bridge contract. Reconnect only the Shin MCP integration to load its new tools and transport.");
    std::set<std::string> names;
    Json fingerprint = Json::array({directory_identity(prefix),stamp(prefix/runtime_path)});
    std::uint64_t total = 0;
    for (const auto& entry : manifest.at("files")) {
        require_runtime(entry.is_object() && entry.size() == 3 && entry.contains("path") && entry.at("path").is_string() &&
            entry.contains("size") && entry.at("size").is_number_unsigned() && entry.contains("sha256") && entry.at("sha256").is_string());
        const auto name = entry.at("path").get<std::string>();
        const auto path = fs::path(name);
        require_runtime(!path.empty() && !path.is_absolute() && path.lexically_normal() == path &&
            name == path.generic_string() && name.size() <= 512 && name.find_first_of("\\:\r\n") == std::string::npos && name.find('\0') == std::string::npos &&
            std::none_of(path.begin(),path.end(),[](const auto& part){ return part == "." || part == ".." || part.empty() || part.string().back() == '.' || part.string().back() == ' '; }) &&
            (name.starts_with("bin/") || name == "share/shin-context/VERSION" || name == "share/shin-observer/VERSION") &&
            names.insert(name).second && entry.at("size").get<std::uint64_t>() <= max_runtime_file);
        const auto hash = entry.at("sha256").get<std::string>();
        require_runtime(hash.size() == 64 && hash.find_first_not_of("0123456789abcdef") == std::string::npos);
        total += entry.at("size").get<std::uint64_t>();
        require_runtime(total <= 512U * 1024 * 1024);
        fingerprint.push_back(Json::array({name,stamp(prefix/path)}));
    }
#ifdef _WIN32
    constexpr auto suffix = ".exe";
#else
    constexpr auto suffix = "";
#endif
    for (const auto* name : {"shin-ce","shin-observer","shin-context","shin-mcp"})
        require_runtime(names.contains(std::string("bin/")+name+suffix));
    require_runtime(names.contains("share/shin-context/VERSION") && names.contains("share/shin-observer/VERSION"));
    // Unlisted runtime libraries/executables must never join a verified cohort.
    for (const auto& entry : fs::recursive_directory_iterator(prefix/"bin"))
        if (!entry.is_directory()) {
            const auto name = entry.path().lexically_relative(prefix).generic_string();
            require_runtime(names.contains(name) || name == "bin/shin-monitor"); // Homebrew's non-runtime launcher.
        }
    result.fingerprint = digest(fingerprint.dump());
    return result;
}
void verify(const fs::path& prefix, const Runtime& runtime) {
    for (const auto& entry : runtime.manifest.at("files")) {
        const auto text = runtime_file(prefix / entry.at("path").get<std::string>(),max_runtime_file);
        require_runtime(text.size() == entry.at("size").get<std::size_t>() && digest(text) == entry.at("sha256").get<std::string>());
    }
    auto version = [&](const char* path) {
        auto value = runtime_file(prefix/path,128);
        if (!value.empty() && value.back() == '\n') value.pop_back();
        return value;
    };
    require_runtime(version("share/shin-context/VERSION") == runtime.manifest.at("core_version").get<std::string>() &&
        version("share/shin-observer/VERSION") == runtime.manifest.at("observer_version").get<std::string>() &&
        inspect(prefix).fingerprint == runtime.fingerprint);
}
}

struct LocalMcpBridge::Impl {
    fs::path locator, home;
    std::string home_identity, owner_identity;
    bool packaged = false, native = false;
    std::string verified_fingerprint;
    std::mutex mutex;
    Impl(const fs::path& executable, const fs::path& data_home)
        : locator(homebrew_managed() ? fs::path(SHIN_OBSERVER_HOMEBREW_PREFIX) : package_payload_prefix(executable)),
          home(data_home), home_identity(directory_identity(home,true)) {
        const auto prefix = fs::canonical(locator);
        packaged = fs::exists(prefix/runtime_path);
        native = fs::exists(prefix/".shin-installation.json");
        if (native) owner_identity = directory_identity(fs::path(parse_json(read_text(prefix/".shin-installation.json",kMaximumStateBytes,true)).at("control").get<std::string>()),true);
    }
    Runtime current() {
        require_runtime(directory_identity(home,true) == home_identity);
        const auto prefix = fs::canonical(locator);
        require_runtime(fs::exists(prefix/".shin-installation.json") == native);
        if (native) {
            const auto receipt = parse_json(read_text(prefix/".shin-installation.json",kMaximumStateBytes,true));
            require_runtime(directory_identity(fs::path(receipt.at("control").get<std::string>()),true) == owner_identity &&
                installed_local_home(prefix/"bin/shin-mcp") == home &&
                digest(read_text(prefix/"shin-kit.json",kMaximumStateBytes,false)) == receipt.at("manifest_sha256").get<std::string>());
            const auto kit = parse_json(read_text(prefix/"shin-kit.json",kMaximumStateBytes,false));
            const auto found = std::find_if(kit.at("files").begin(),kit.at("files").end(),[](const auto& file){ return file.at("path") == runtime_path; });
            require_runtime(found != kit.at("files").end() && digest(runtime_file(prefix/runtime_path,kMaximumStateBytes)) == found->at("sha256").template get<std::string>());
        }
        auto result = inspect(prefix);
        if (result.fingerprint != verified_fingerprint) {
            verify(prefix,result);
            verified_fingerprint = result.fingerprint;
        }
        return result;
    }
};

LocalMcpBridge::LocalMcpBridge(const fs::path& executable, const fs::path& home)
    : impl_(std::make_unique<Impl>(executable,home)) {}
LocalMcpBridge::~LocalMcpBridge() = default;
Json LocalMcpBridge::request(const LocalProjectRequest& request, const std::function<bool()>& cancelled) {
    if (!impl_->packaged) return request_local_service(request,cancelled);
    Runtime runtime;
    {
        const std::lock_guard lock(impl_->mutex);
        require_runtime(request.home == impl_->home);
        runtime = impl_->current();
    }
    const VerifiedMcpRuntime identity(runtime.manifest.at("observer_version"),runtime.manifest.at("core_version"),runtime.binding);
    auto result = request_verified_mcp_service(request,identity,cancelled);
    {
        const std::lock_guard lock(impl_->mutex);
        if (impl_->current().fingerprint != runtime.fingerprint)
            throw Error("mcp-runtime-changed", "Shin changed during this request. The result was withheld; make a fresh useful request after the update. Check saved state before repeating a checkpoint write.");
    }
    return result;
}
Json local_mcp_runtime_binding(const fs::path& executable) {
    const auto prefix = package_payload_prefix(executable);
    if (!fs::exists(prefix/runtime_path)) return nullptr;
    const auto runtime = inspect(prefix);
    verify(prefix,runtime);
    require_runtime(runtime.manifest.at("observer_version") == SHIN_OBSERVER_VERSION && runtime.manifest.at("core_version") == SHIN_VERSION);
    return runtime.binding;
}
std::string local_mcp_adapter_contract() { return SHIN_MCP_ADAPTER_CONTRACT; }
}
