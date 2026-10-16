#include "shin/server/config.h"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <set>

namespace shin::server {
namespace {

[[noreturn]] void invalid(std::string code = "invalid-request") {
    throw ServiceError(400, std::move(code), "The request does not satisfy the contract.");
}

class FileDescriptor {
public:
    explicit FileDescriptor(int value) : value_(value) {}
    ~FileDescriptor() { if (value_ >= 0) ::close(value_); }
    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;
    int get() const { return value_; }
private:
    int value_;
};

bool unchanged_private_file(const struct stat& before, const struct stat& after) {
#if defined(__APPLE__)
    const auto modified_before = before.st_mtimespec;
    const auto modified_after = after.st_mtimespec;
    const auto changed_before = before.st_ctimespec;
    const auto changed_after = after.st_ctimespec;
#else
    const auto modified_before = before.st_mtim;
    const auto modified_after = after.st_mtim;
    const auto changed_before = before.st_ctim;
    const auto changed_after = after.st_ctim;
#endif
    return before.st_size == after.st_size && before.st_mode == after.st_mode &&
           before.st_uid == after.st_uid && before.st_gid == after.st_gid &&
           modified_before.tv_sec == modified_after.tv_sec && modified_before.tv_nsec == modified_after.tv_nsec &&
           changed_before.tv_sec == changed_after.tv_sec && changed_before.tv_nsec == changed_after.tv_nsec;
}

std::string read_private_file(const std::filesystem::path& path, std::size_t limit) {
    if (!path.is_absolute()) invalid("invalid-config");
    FileDescriptor file(::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK));
    struct stat before{};
    if (file.get() < 0 || ::fstat(file.get(), &before) != 0 ||
        !S_ISREG(before.st_mode) || before.st_uid != ::geteuid() ||
        (before.st_mode & 0077) != 0 || before.st_size <= 0 ||
        static_cast<std::uintmax_t>(before.st_size) > limit) invalid("invalid-config");
    std::string data;
    data.reserve(static_cast<std::size_t>(before.st_size));
    std::array<char, 4096> buffer{};
    for (;;) {
        const auto count = ::read(file.get(), buffer.data(), buffer.size());
        if (count == 0) break;
        if (count < 0) {
            if (errno == EINTR) continue;
            invalid("invalid-config");
        }
        data.append(buffer.data(), static_cast<std::size_t>(count));
        if (data.size() > limit) invalid("invalid-config");
    }
    struct stat after{};
    if (::fstat(file.get(), &after) != 0 || !unchanged_private_file(before, after) ||
        data.size() != static_cast<std::size_t>(before.st_size)) invalid("invalid-config");
    return data;
}

std::size_t integer(const Json& object, const char* key, std::size_t low, std::size_t high) {
    if (!object.contains(key) || !object[key].is_number_unsigned()) invalid();
    const auto value = object[key].get<std::uint64_t>();
    if (value < low || value > high) invalid();
    return static_cast<std::size_t>(value);
}

std::uintmax_t unsigned_integer(const Json& object, const char* key, std::uintmax_t low, std::uintmax_t high) {
    if (!object.contains(key) || !object[key].is_number_unsigned()) invalid();
    const auto value = object[key].get<std::uint64_t>();
    if (value < low || value > high) invalid();
    return static_cast<std::uintmax_t>(value);
}

bool contains_path(const std::filesystem::path& parent, const std::filesystem::path& child) {
    const auto [first, unused] = std::mismatch(parent.begin(), parent.end(), child.begin(), child.end());
    (void)unused;
    return first == parent.end();
}

std::filesystem::path canonical_config_path(const std::filesystem::path& input) {
    if (!input.is_absolute()) invalid();
    std::error_code error;
    const auto canonical = std::filesystem::canonical(input, error);
    if (error || canonical != input.lexically_normal()) invalid();
    return canonical;
}

std::filesystem::path canonical_directory(const Json& value, std::string_view field) {
    const std::filesystem::path input(require_text(value, field, 4096));
    std::error_code error;
    const auto canonical = canonical_config_path(input);
    if (canonical == canonical.root_path() || !std::filesystem::is_directory(canonical, error) || error)
        invalid();
    return canonical;
}

std::filesystem::path canonical_private_directory(const Json& value, std::string_view field) {
    const auto directory = canonical_directory(value, field);
    FileDescriptor descriptor(::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    struct stat status{};
    if (descriptor.get() < 0 || ::fstat(descriptor.get(), &status) != 0 || !S_ISDIR(status.st_mode) ||
        status.st_uid != ::geteuid() || (status.st_mode & 0077) != 0)
        invalid();
    return directory;
}

Digest decode_digest(const std::string& value) {
    if (value.size() != 64) invalid("invalid-config");
    Digest result{};
    auto nibble = [](char c) -> unsigned char {
        if (c >= '0' && c <= '9') return static_cast<unsigned char>(c - '0');
        if (c >= 'a' && c <= 'f') return static_cast<unsigned char>(c - 'a' + 10);
        invalid("invalid-config");
    };
    for (std::size_t i = 0; i < result.size(); ++i)
        result[i] = static_cast<unsigned char>((nibble(value[i * 2]) << 4) | nibble(value[i * 2 + 1]));
    return result;
}

StoreConfig load_database_config(const Json& database, std::string source_host_id, std::size_t pool_size) {
    require_fields(database, {"connection_file", "expected_name", "server_id", "runtime_role"});
    const auto connection_file = canonical_config_path(
        std::filesystem::path(require_text(database, "connection_file", 4096)));
    StoreConfig result;
    result.connection_string = read_private_file(connection_file, 16384);
    result.expected_database = require_text(database, "expected_name", 63);
    result.expected_server_id = require_text(database, "server_id", 32);
    if (result.expected_server_id.size() != 32 ||
        !std::all_of(result.expected_server_id.begin(), result.expected_server_id.end(), [](char value) {
            return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f');
        }))
        invalid();
    result.runtime_role = require_text(database, "runtime_role", 63);
    result.pool_size = pool_size;
    result.source_host_id = std::move(source_host_id);
    return result;
}

} // namespace

Json parse_bounded_json(std::string_view text, std::size_t maximum_bytes) {
    if (text.empty() || text.size() > maximum_bytes) invalid("invalid-json");
    std::vector<std::set<std::string>> objects;
    try {
        auto callback = [&](int depth, Json::parse_event_t event, Json& parsed) {
            if (depth > 16) invalid("invalid-json");
            if (event == Json::parse_event_t::object_start) objects.emplace_back();
            if (event == Json::parse_event_t::key &&
                (objects.empty() || !objects.back().insert(parsed.get<std::string>()).second))
                invalid("invalid-json");
            if (event == Json::parse_event_t::object_end) objects.pop_back();
            return true;
        };
        return Json::parse(text.begin(), text.end(), callback);
    } catch (const Json::exception&) {
        invalid("invalid-json");
    }
}

Json load_private_json(const std::filesystem::path& path, std::size_t maximum_bytes) {
    const auto canonical = canonical_config_path(path);
    return parse_bounded_json(read_private_file(canonical, maximum_bytes), maximum_bytes);
}

void require_fields(const Json& value, std::initializer_list<std::string_view> fields) {
    if (!value.is_object() || value.size() != fields.size()) invalid();
    for (const auto name : fields) if (!value.contains(name)) invalid();
}

std::string require_text(const Json& value, std::string_view field, std::size_t maximum_bytes) {
    const auto item = value.find(field);
    if (item == value.end() || !item->is_string()) invalid();
    auto text = item->get<std::string>();
    if (text.empty() || text.size() > maximum_bytes ||
        std::any_of(text.begin(), text.end(), [](unsigned char c) { return c < 32 || c == 127; })) invalid();
    return text;
}

bool valid_identifier(std::string_view value) {
    if (value.empty() || value.size() > 63 || value.front() < 'a' || value.front() > 'z') return false;
    return std::all_of(value.begin(), value.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
    });
}

Digest sha256(std::string_view input) {
    Digest digest{};
    unsigned int length = 0;
    if (EVP_Digest(input.data(), input.size(), digest.data(), &length, EVP_sha256(), nullptr) != 1 ||
        length != digest.size()) throw ServiceError(500, "crypto-error", "Digest calculation failed.");
    return digest;
}

std::string digest_hex(const Digest& digest) {
    constexpr char hex[] = "0123456789abcdef";
    std::string output;
    output.reserve(digest.size() * 2);
    for (auto value : digest) {
        output.push_back(hex[value >> 4]);
        output.push_back(hex[value & 15]);
    }
    return output;
}

Config load_config(const std::filesystem::path& path) {
    try {
        const auto value = load_private_json(path, 65536);
        auto required = value;
        required.erase("oidc");
        require_fields(required, {"schema_version", "source_host_id", "listen", "database", "allowed_roots", "principals", "retrieval", "preparations", "observability", "knowledge", "journal_writer", "workers", "queue_limit"});
        if (value["schema_version"] != kServiceConfigSchema) invalid();
        Config result;
        if (value.contains("oidc")) {
            const auto& input = value.at("oidc");
            require_fields(input, {"issuer", "audience", "client_id", "ca_file"});
            OidcProviderOptions options;
            options.trust = {require_text(input, "issuer", 2048), require_text(input, "audience", 255),
                             require_text(input, "client_id", 255)};
            if (!options.trust.issuer.starts_with("https://") || options.trust.audience == options.trust.client_id)
                invalid();
            if (!input.at("ca_file").is_string()) invalid();
            const auto ca = input.at("ca_file").get<std::string>();
            if (!ca.empty()) {
                options.ca_file = require_text(input, "ca_file", 4096);
                if (!options.ca_file.is_absolute() || !std::filesystem::is_regular_file(options.ca_file)) invalid();
            }
            result.oidc = std::move(options);
        }
        const auto& observability = value["observability"];
        require_fields(observability, {"max_retained_requests", "max_query_bytes", "capture_query_projects"});
        result.observability.max_retained_requests = integer(observability, "max_retained_requests", 0, 256);
        result.observability.max_query_bytes = integer(observability, "max_query_bytes", 1, 8192);
        const auto& capture_projects = observability["capture_query_projects"];
        if (!capture_projects.is_array() || capture_projects.size() > 32 ||
            (result.observability.max_retained_requests == 0 && !capture_projects.empty()))
            invalid();
        std::set<std::string> unique_capture_projects;
        for (const auto& project : capture_projects) {
            if (!project.is_string()) invalid();
            const auto id = project.get<std::string>();
            if (!valid_identifier(id) || !unique_capture_projects.insert(id).second) invalid();
            result.observability.capture_query_projects.push_back(id);
        }
        const auto source_host_id = require_text(value, "source_host_id", 63);
        if (!valid_identifier(source_host_id)) invalid();
        require_fields(value["listen"], {"host", "port"});
        result.host = require_text(value["listen"], "host", 64);
        if (result.host != "127.0.0.1") invalid();
        result.port = static_cast<int>(integer(value["listen"], "port", 1024, 65535));
        result.workers = integer(value, "workers", 1, 16);
        result.queue_limit = integer(value, "queue_limit", 1, 256);
        result.database = load_database_config(value["database"], source_host_id, result.workers);
        const auto& roots = value["allowed_roots"];
        if (!roots.is_array() || roots.empty() || roots.size() > 32) invalid();
        for (const auto& root : roots) {
            if (!root.is_string()) invalid();
            const auto root_text = require_text(Json{{"root", root}}, "root", 4096);
            const auto canonical = canonical_directory(Json{{"root", root_text}}, "root");
            if (std::find(result.allowed_roots.begin(), result.allowed_roots.end(), canonical) != result.allowed_roots.end()) invalid();
            result.allowed_roots.push_back(canonical);
        }
        const auto& principals = value["principals"];
        if (!principals.is_array() || principals.empty() || principals.size() > 32) invalid();
        std::set<std::string> ids;
        std::set<std::string> hashes;
        bool has_admin = false;
        for (const auto& item : principals) {
            if (item.contains("receipt_producer"))
                require_fields(item, {"id", "role", "token_sha256", "project_ids", "receipt_producer"});
            else
                require_fields(item, {"id", "role", "token_sha256", "project_ids"});
            Credential credential;
            credential.principal.id = require_text(item, "id", 63);
            if (!valid_identifier(credential.principal.id) || !ids.insert(credential.principal.id).second) invalid();
            const auto role = require_text(item, "role", 16);
            if (role != "admin" && role != "reader") invalid();
            credential.principal.admin = role == "admin";
            if (item.contains("receipt_producer")) {
                if (!item.at("receipt_producer").is_boolean()) invalid();
                credential.principal.receipt_producer = item.at("receipt_producer").get<bool>();
            }
            has_admin |= credential.principal.admin;
            const auto hash = require_text(item, "token_sha256", 64);
            if (!hashes.insert(hash).second) invalid();
            credential.token_hash = decode_digest(hash);
            const auto& projects = item["project_ids"];
            if (!projects.is_array() || projects.size() > 128 || (credential.principal.admin && !projects.empty())) invalid();
            std::set<std::string> unique_projects;
            for (const auto& project : projects) {
                if (!project.is_string()) invalid();
                const auto id = project.get<std::string>();
                if (!valid_identifier(id) || !unique_projects.insert(id).second) invalid();
                credential.principal.project_ids.push_back(id);
            }
            result.credentials.push_back(std::move(credential));
        }
        if (!has_admin) invalid();
        const auto& retrieval = value["retrieval"];
        require_fields(retrieval, {"routes", "max_payload_bytes", "max_generation_bytes", "max_file_bytes", "max_generations"});
        result.retrieval.max_payload_bytes = unsigned_integer(
            retrieval, "max_payload_bytes", 1, 1024ULL * 1024 * 1024);
        result.retrieval.max_generation_bytes = unsigned_integer(
            retrieval, "max_generation_bytes", 1, result.retrieval.max_payload_bytes);
        result.retrieval.max_file_bytes = unsigned_integer(
            retrieval, "max_file_bytes", 1, result.retrieval.max_generation_bytes);
        result.retrieval.max_generations = integer(retrieval, "max_generations", 1, 32);
        const auto& routes = retrieval["routes"];
        if (!routes.is_array() || routes.size() > 32) invalid();
        std::set<std::string> route_ids;
        for (const auto& item : routes) {
            require_fields(item, {"id", "project_id", "workspace_id", "root", "profile", "profile_dir"});
            RetrievalRoute route;
            route.id = require_text(item, "id", 63);
            route.project_id = require_text(item, "project_id", 63);
            route.workspace_id = require_text(item, "workspace_id", 63);
            route.source_host_id = result.database.source_host_id;
            route.profile = require_text(item, "profile", 63);
            if (!valid_identifier(route.id) || !valid_identifier(route.project_id) || !valid_identifier(route.workspace_id) ||
                !valid_identifier(route.profile) ||
                !route_ids.insert(route.id).second)
                invalid();
            const auto workspace = workspace_input(result, Json{{"id", route.workspace_id},
                {"display_name", route.workspace_id}, {"root", require_text(item, "root", 4096)}});
            route.root = workspace.root;
            route.root_identity = workspace.root_identity;
            route.profile_dir = canonical_directory(item, "profile_dir");
            result.retrieval.routes.push_back(std::move(route));
        }
        const auto& preparations = value["preparations"];
        require_fields(preparations, {"enabled", "state_dir", "max_retained_jobs", "lease_seconds"});
        if (!preparations["enabled"].is_boolean()) invalid();
        result.preparations.enabled = preparations["enabled"].get<bool>();
        result.preparations.state_dir = canonical_private_directory(preparations, "state_dir");
        result.preparations.max_retained_jobs = integer(preparations, "max_retained_jobs", 1, 64);
        result.preparations.lease_seconds = static_cast<int>(integer(preparations, "lease_seconds", 3, 300));
        const auto& knowledge = value["knowledge"];
        require_fields(knowledge, {"enabled", "principal_id", "route_ids", "interval_seconds", "max_age_seconds", "prepare_on_drift"});
        if (!knowledge["enabled"].is_boolean() || !knowledge["prepare_on_drift"].is_boolean() ||
            !knowledge["principal_id"].is_string() || !knowledge["route_ids"].is_array()) invalid();
        auto& policy = result.knowledge;
        policy.enabled = knowledge["enabled"].get<bool>();
        policy.prepare_on_drift = knowledge["prepare_on_drift"].get<bool>();
        policy.principal_id = knowledge["principal_id"].get<std::string>();
        policy.interval_seconds = static_cast<int>(integer(knowledge, "interval_seconds", 1, 3600));
        policy.max_age_seconds = static_cast<int>(integer(knowledge, "max_age_seconds", 1, 86400));
        if (policy.max_age_seconds < policy.interval_seconds || knowledge["route_ids"].size() > 8) invalid();
        std::set<std::string> targets;
        for (const auto& id : knowledge["route_ids"]) {
            if (!id.is_string() || !route_ids.contains(id.get<std::string>()) || !targets.insert(id.get<std::string>()).second) invalid();
            policy.route_ids.push_back(id.get<std::string>());
        }
        if (policy.enabled) {
            const auto actor = std::find_if(result.credentials.begin(), result.credentials.end(), [&](const Credential& credential) {
                return credential.principal.id == policy.principal_id && credential.principal.admin;
            });
            if (actor == result.credentials.end() || policy.route_ids.empty() ||
                policy.route_ids.size() > result.retrieval.max_generations ||
                policy.route_ids.size() > result.retrieval.max_payload_bytes / result.retrieval.max_generation_bytes ||
                (policy.prepare_on_drift && !result.preparations.enabled)) invalid();
        } else if (!policy.route_ids.empty() || !policy.principal_id.empty() || policy.prepare_on_drift) invalid();
        const auto& journal = value["journal_writer"];
        require_fields(journal, {"enabled", "principal_id", "interval_ms", "max_retry_ms", "flush_on_shutdown"});
        if (!journal["enabled"].is_boolean() || !journal["flush_on_shutdown"].is_boolean() ||
            !journal["principal_id"].is_string()) invalid();
        auto& writer = result.journal_writer;
        writer.enabled = journal["enabled"].get<bool>();
        writer.principal_id = journal["principal_id"].get<std::string>();
        writer.interval_ms = static_cast<int>(integer(journal, "interval_ms", 100, 60000));
        writer.max_retry_ms = static_cast<int>(integer(journal, "max_retry_ms", writer.interval_ms, 300000));
        writer.flush_on_shutdown = journal["flush_on_shutdown"].get<bool>();
        if (writer.enabled) {
            const auto actor = std::find_if(result.credentials.begin(), result.credentials.end(), [&](const Credential& credential) {
                return credential.principal.id == writer.principal_id && credential.principal.admin;
            });
            if (actor == result.credentials.end() || result.observability.max_retained_requests == 0) invalid();
        } else if (!writer.principal_id.empty()) invalid();
        constexpr std::uintmax_t max_preparation_commitment = 1024ULL * 1024 * 1024;
        if (result.retrieval.max_generation_bytes >
            max_preparation_commitment / result.preparations.max_retained_jobs)
            invalid();
        for (const auto& route : result.retrieval.routes) {
            if (contains_path(route.root, result.preparations.state_dir) ||
                contains_path(result.preparations.state_dir, route.root))
                invalid();
        }
        return result;
    } catch (const std::exception&) {
        throw ServiceError(400, "invalid-config", "Configuration is invalid, unavailable or insufficiently protected.");
    }
}

StoreConfig load_recovery_config(const std::filesystem::path& path) {
    try {
        const auto value = load_private_json(path, 65536);
        require_fields(value, {"schema_version", "database"});
        if (value["schema_version"] != kRecoveryConfigSchema) invalid();
        return load_database_config(value["database"], "recovery", 1);
    } catch (const std::exception&) {
        throw ServiceError(400, "invalid-config", "Configuration is invalid, unavailable or insufficiently protected.");
    }
}

const Principal& authenticate(const Config& config, std::string_view token) {
    if (token.size() < 32 || token.size() > 256 ||
        std::any_of(token.begin(), token.end(), [](unsigned char c) { return c <= 32 || c >= 127; }))
        throw ServiceError(401, "unauthorized", "Authentication is required.");
    const auto digest = sha256(token);
    const Principal* found = nullptr;
    for (const auto& credential : config.credentials)
        if (CRYPTO_memcmp(digest.data(), credential.token_hash.data(), digest.size()) == 0)
            found = &credential.principal;
    if (!found) throw ServiceError(401, "unauthorized", "Authentication is required.");
    return *found;
}

ProjectInput project_input(const Config& config, const Json& value) {
    (void)config;
    require_fields(value, {"id", "display_name"});
    ProjectInput result;
    result.id = require_text(value, "id", 63);
    if (!valid_identifier(result.id)) invalid();
    result.display_name = require_text(value, "display_name", 120);
    return result;
}

WorkspaceInput workspace_input(const Config& config, const Json& value) {
    require_fields(value, {"id", "display_name", "root"});
    WorkspaceInput result;
    result.id = require_text(value, "id", 63);
    if (!valid_identifier(result.id)) invalid();
    result.display_name = require_text(value, "display_name", 120);
    result.source_host_id = config.database.source_host_id;
    const std::filesystem::path input(require_text(value, "root", 4096));
    std::error_code error;
    const auto root = std::filesystem::canonical(input, error);
    if (!input.is_absolute() || error || !std::filesystem::is_directory(root, error) || error)
        invalid("invalid-root");
    if (std::none_of(config.allowed_roots.begin(), config.allowed_roots.end(),
                     [&](const auto& allowed) { return contains_path(allowed, root); }))
        invalid("root-not-allowed");
    FileDescriptor directory(::open(root.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    struct stat identity{};
    if (directory.get() < 0 || ::fstat(directory.get(), &identity) != 0 || !S_ISDIR(identity.st_mode))
        invalid("invalid-root");
    result.root = root.string();
    result.root_identity = std::to_string(static_cast<std::uintmax_t>(identity.st_dev)) + ":" +
                           std::to_string(static_cast<std::uintmax_t>(identity.st_ino));
    return result;
}

} // namespace shin::server
