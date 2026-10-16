#include "shin/observer/observer.h"
#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#include <limits>
#else
#include <openssl/evp.h>
#endif
#include <array>
#include <algorithm>
#include <set>

namespace shin::observer {
namespace {
void fields(const Json& value, const std::set<std::string>& expected) {
    if (!value.is_object() || value.size() != expected.size())
        throw Error("invalid-config", "Configuration fields are invalid.");
    for (const auto& field : expected)
        if (!value.contains(field)) throw Error("invalid-config", "Configuration fields are invalid.");
}

std::string text(const Json& value, const std::string& name, std::size_t maximum) {
    if (!value.at(name).is_string()) throw Error("invalid-config", "Configuration value is invalid.");
    auto result = value.at(name).get<std::string>();
    if (result.empty() || result.size() > maximum || result.find('\0') != std::string::npos)
        throw Error("invalid-config", "Configuration value is invalid.");
    return result;
}

std::string identifier(const Json& value, const std::string& name) {
    const auto result = text(value, name, 63);
    if (result.front() < 'a' || result.front() > 'z' ||
        result.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789-") != std::string::npos)
        throw Error("invalid-config", "Configuration identity is invalid.");
    return result;
}

fs::path path(const Json& value, const std::string& name) {
    const fs::path result(text(value, name, 4096));
    std::error_code error;
    if (!result.is_absolute() || result == result.root_path() || result.lexically_normal() != result ||
        fs::weakly_canonical(result, error) != result || error)
        throw Error("invalid-config", "Paths must be absolute and normalized.");
    return result;
}

std::uint64_t number(const Json& value, const std::string& name, std::uint64_t low, std::uint64_t high) {
    const auto& result = value.at(name);
    if (!result.is_number_integer() || result < low || result > high)
        throw Error("invalid-config", "Configuration limit is invalid.");
    return result.get<std::uint64_t>();
}

bool overlaps(const fs::path& a, const fs::path& b) {
    const auto [left, right] = std::mismatch(a.begin(), a.end(), b.begin(), b.end());
    return left == a.end() || right == b.end();
}
} // namespace

std::string digest(const std::string& text) {
#ifdef _WIN32
    std::array<unsigned char, 32> bytes{};
    constexpr unsigned int count = 32;
    if (text.size() > std::numeric_limits<ULONG>::max() ||
        BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0,
            reinterpret_cast<PUCHAR>(const_cast<char*>(text.data())), static_cast<ULONG>(text.size()), bytes.data(), count) != 0)
        throw Error("identity-failed", "Could not compute local identity.");
#else
    std::array<unsigned char, EVP_MAX_MD_SIZE> bytes{};
    unsigned int count = 0;
    if (EVP_Digest(text.data(), text.size(), bytes.data(), &count, EVP_sha256(), nullptr) != 1 || count != 32)
        throw Error("identity-failed", "Could not compute local identity.");
#endif
    std::string result;
    for (unsigned int i = 0; i < count; ++i) {
        result.push_back("0123456789abcdef"[bytes[i] >> 4]);
        result.push_back("0123456789abcdef"[bytes[i] & 15]);
    }
    return result;
}

Config load_config(const fs::path& file) {
    if (!file.is_absolute() || file.lexically_normal() != file)
        throw Error("invalid-config", "Configuration path must be absolute and normalized.");
    const auto value = parse_json(read_text(file, 65536, true));
    return config_from_json(value, file);
}

Config config_from_json(const Json& value, const fs::path& file) {
    if (!file.is_absolute() || file.lexically_normal() != file)
        throw Error("invalid-config", "Configuration path must be absolute and normalized.");
    fields(value, {"schema_version", "observer_id", "source_host_id", "project_id", "workspace_id", "root",
        "profile", "profile_dir", "out_dir", "state_dir", "poll_interval_ms", "max_snapshot_bytes", "max_file_bytes", "outbox"});
    if (value.at("schema_version") != "shin.observer-config.v2")
        throw Error("invalid-config", "Unsupported Observer configuration.");
    Config result;
    result.root = path(value, "root");
    result.out_dir = path(value, "out_dir");
    result.profile_dir = path(value, "profile_dir");
    result.state_dir = path(value, "state_dir");
    result.profile = identifier(value, "profile");
    result.poll_interval_ms = number(value, "poll_interval_ms", 100, 60000);
    result.max_snapshot_bytes = number(value, "max_snapshot_bytes", 1, 1024ULL * 1024 * 1024);
    result.max_file_bytes = number(value, "max_file_bytes", 1, result.max_snapshot_bytes);
    const auto& outbox = value.at("outbox");
    fields(outbox, {"enabled", "max_entries", "max_bytes"});
    if (!outbox.at("enabled").is_boolean()) throw Error("invalid-config", "Outbox opt-in must be explicit.");
    result.outbox_enabled = outbox.at("enabled").get<bool>();
    result.outbox_max_entries = number(outbox, "max_entries", 1, 128);
    result.outbox_max_bytes = number(outbox, "max_bytes", 4096, 192 * 1024);
    if (overlaps(result.state_dir, result.root) || overlaps(result.state_dir, result.out_dir) ||
        overlaps(result.state_dir, result.profile_dir) || overlaps(result.root, result.out_dir) ||
        overlaps(file, result.root) || overlaps(file, result.out_dir))
        throw Error("invalid-config", "Source, publication and private Observer state must be separate.");
    result.binding = {{"observer_id", identifier(value, "observer_id")},
        {"source_host_id", identifier(value, "source_host_id")}, {"project_id", identifier(value, "project_id")},
        {"workspace_id", identifier(value, "workspace_id")}, {"root", result.root.string()},
        {"profile", result.profile}, {"profile_dir", result.profile_dir.string()}, {"out_dir", result.out_dir.string()},
        {"state_dir", result.state_dir.string()}, {"state_identity", directory_identity(result.state_dir, true)},
        {"outbox_policy", outbox}};
    result.binding_key = digest(result.binding.dump());
    return result;
}
} // namespace shin::observer
