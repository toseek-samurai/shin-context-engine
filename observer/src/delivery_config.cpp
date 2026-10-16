#include "shin/observer/delivery.h"
#include <algorithm>
#include <set>

namespace shin::observer {
namespace {
[[noreturn]] void invalid() { throw Error("invalid-delivery-config", "Delivery requires an explicit private loopback endpoint and identity."); }
bool within(const fs::path& child, const fs::path& parent) {
    return std::mismatch(parent.begin(), parent.end(), child.begin(), child.end()).first == parent.end();
}
void private_path(const Config& config, const fs::path& path) {
    if (!path.is_absolute() || path.lexically_normal() != path || within(path, config.root) ||
        within(path, config.out_dir) || within(path, config.profile_dir)) invalid();
}
std::uint64_t number(const Json& value, std::uint64_t low, std::uint64_t high) {
    if (!value.is_number_integer() || value < low || value > high) invalid();
    return value.get<std::uint64_t>();
}
} // namespace

DeliveryConfig load_delivery_config(const Config& config, const fs::path& file) {
    if (!config.outbox_enabled) invalid();
    private_path(config, file);
    const auto value = parse_json(read_text(file, 8192, true));
    const std::set<std::string> keys{"schema_version", "host", "port", "server_id", "principal_id", "token_file", "request_timeout_ms"};
    if (!value.is_object() || value.size() != keys.size() ||
        !std::all_of(keys.begin(), keys.end(), [&](const auto& key) { return value.contains(key); })) invalid();
    if (value.at("schema_version") != "shin.observer-delivery-config.v1" || value.at("host") != "127.0.0.1") invalid();
    for (const auto* key : {"server_id", "principal_id", "token_file"}) if (!value.at(key).is_string()) invalid();
    const auto& server = value.at("server_id").get_ref<const std::string&>();
    const auto& principal = value.at("principal_id").get_ref<const std::string&>();
    if (server.size() != 32 || server.find_first_not_of("0123456789abcdef") != std::string::npos ||
        principal.empty() || principal.size() > 63 || principal.front() < 'a' || principal.front() > 'z' ||
        principal.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789-") != std::string::npos) invalid();
    const auto token_path = value.at("token_file").get<std::string>();
    if (token_path.size() > 4096 || token_path.find('\0') != std::string::npos) invalid();
    private_path(config, token_path);
    DeliveryConfig result;
    result.token = read_text(token_path, 257, true);
    if (!result.token.empty() && result.token.back() == '\n') result.token.pop_back();
    if (result.token.size() < 32 || result.token.size() > 256 ||
        result.token.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._~-") != std::string::npos) invalid();
    result.destination = {{"host", "127.0.0.1"}, {"port", number(value.at("port"), 1024, 65535)},
        {"server_id", server}, {"principal_id", principal}};
    result.timeout_ms = number(value.at("request_timeout_ms"), 100, 10000);
    return result;
}

void bind_delivery(Json& queue, const DeliveryConfig& config) {
    if (queue.is_null()) throw Error("outbox-disabled", "Delivery requires an enabled outbox.");
    if (!queue.at("destination").is_null() && queue.at("destination") != config.destination)
        throw Error("delivery-binding-mismatch", "Existing delivery destination cannot be changed implicitly.");
    queue["destination"] = config.destination;
}
} // namespace shin::observer
