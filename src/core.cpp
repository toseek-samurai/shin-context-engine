#include "context_internal.h"

#ifdef _WIN32
#include "shin/windows/file.h"
#endif
#ifndef _WIN32
#include <sys/resource.h>
#endif

namespace shin::context {

double elapsed_ms_since(SteadyClock::time_point start) {
    const std::chrono::duration<double, std::milli> elapsed = SteadyClock::now() - start;
    return elapsed.count();
}

std::uint64_t process_peak_rss_bytes() noexcept {
#ifdef _WIN32
    return 0;
#else
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0 || usage.ru_maxrss <= 0) {
        return 0;
    }
    const std::uint64_t resident = static_cast<std::uint64_t>(usage.ru_maxrss);
#ifdef __APPLE__
    return resident;
#else
    return resident * 1024U;
#endif
#endif
}

std::string read_file(const fs::path& path) {
    errno = 0;
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("Cannot read " + path.string());
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

std::string trim(std::string value) {
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), [](char c) {
        return !is_ascii_space(static_cast<unsigned char>(c));
    }));
    value.erase(std::find_if(value.rbegin(), value.rend(), [](char c) {
        return !is_ascii_space(static_cast<unsigned char>(c));
    }).base(), value.end());
    return value;
}

std::string to_lower(std::string value) {
    for (char& c : value) {
        c = static_cast<char>(ascii_lower(static_cast<unsigned char>(c)));
    }
    return value;
}

bool contains_ascii_case_insensitive(std::string_view haystack, std::string_view lower_needle) noexcept {
    if (lower_needle.empty()) {
        return true;
    }
    if (lower_needle.size() > haystack.size()) {
        return false;
    }
    const unsigned char first = ascii_lower(static_cast<unsigned char>(lower_needle.front()));
    const std::size_t last_start = haystack.size() - lower_needle.size();
    for (std::size_t i = 0; i <= last_start; ++i) {
        if (ascii_lower(static_cast<unsigned char>(haystack[i])) != first) {
            continue;
        }
        bool matched = true;
        for (std::size_t j = 1; j < lower_needle.size(); ++j) {
            if (ascii_lower(static_cast<unsigned char>(haystack[i + j])) !=
                ascii_lower(static_cast<unsigned char>(lower_needle[j]))) {
                matched = false;
                break;
            }
        }
        if (matched) {
            return true;
        }
    }
    return false;
}

bool portable_identifier_valid(std::string_view value) noexcept {
    if (value.empty() || value.size() > 160) {
        return false;
    }
    for (std::size_t index = 0; index < value.size(); ++index) {
        const unsigned char c = static_cast<unsigned char>(value[index]);
        if (is_ascii_lower(c) || is_ascii_digit(c)) {
            continue;
        }
        if (index > 0 && (c == '-' || c == '_' || c == '.')) {
            continue;
        }
        return false;
    }
    const char last = value.back();
    return last != '-' && last != '_' && last != '.';
}

unsigned int worker_count_for_items(std::size_t item_count, std::size_t target_items_per_worker) {
    if (item_count <= 1) {
        return 1;
    }
    unsigned int hardware_threads = std::thread::hardware_concurrency();
    if (hardware_threads == 0) {
        hardware_threads = 1;
    }
    const std::size_t items_per_worker = std::max<std::size_t>(1, target_items_per_worker);
    const std::size_t useful_workers = (item_count - 1) / items_per_worker + 1;
    return static_cast<unsigned int>(std::min<std::size_t>(hardware_threads, useful_workers));
}

void capture_first_exception(std::exception_ptr& first_exception,
                             std::mutex& exception_mutex,
                             std::exception_ptr candidate) {
    std::lock_guard<std::mutex> lock(exception_mutex);
    if (!first_exception) {
        first_exception = candidate;
    }
}

bool starts_with(std::string_view value, std::string_view prefix) noexcept {
    return value.starts_with(prefix);
}

bool ends_with(std::string_view value, std::string_view suffix) noexcept {
    return value.ends_with(suffix);
}

std::vector<std::string> split_lines(const std::string& text) {
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start < text.size()) {
        std::size_t end = text.find('\n', start);
        std::string line = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        lines.push_back(line);
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return lines;
}

std::string compact_whitespace(const std::string& text) {
    std::string out;
    bool previous_space = false;
    for (unsigned char c : text) {
        if (is_ascii_space(c)) {
            if (!previous_space && !out.empty()) {
                out.push_back(' ');
            }
            previous_space = true;
        } else {
            out.push_back(static_cast<char>(c));
            previous_space = false;
        }
    }
    return trim(out);
}

std::string truncate_text(const std::string& text, std::size_t limit) {
    if (text.size() <= limit) {
        return text;
    }
    if (limit <= 3) {
        return text.substr(0, limit);
    }
    return text.substr(0, limit - 3) + "...";
}

std::string json_escape(const std::string& value) {
    if (std::all_of(value.begin(), value.end(), [](unsigned char c) {
            return c >= 0x20 && c < 0x7f && c != '\\' && c != '"';
        })) {
        return value;
    }
    std::ostringstream out;
    for (std::size_t i = 0; i < value.size();) {
        unsigned char c = static_cast<unsigned char>(value[i]);
        switch (c) {
            case '\\':
                out << "\\\\";
                ++i;
                break;
            case '"':
                out << "\\\"";
                ++i;
                break;
            case '\b':
                out << "\\b";
                ++i;
                break;
            case '\f':
                out << "\\f";
                ++i;
                break;
            case '\n':
                out << "\\n";
                ++i;
                break;
            case '\r':
                out << "\\r";
                ++i;
                break;
            case '\t':
                out << "\\t";
                ++i;
                break;
            default:
                if (c < 0x20) {
                    out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(c)
                        << std::dec << std::setfill(' ');
                    ++i;
                } else if (c < 0x80) {
                    out << static_cast<char>(c);
                    ++i;
                } else {
                    auto continuation = [&](std::size_t pos) {
                        return pos < value.size() &&
                               (static_cast<unsigned char>(value[pos]) & 0xC0) == 0x80;
                    };
                    std::size_t length = 0;
                    bool valid = false;
                    if (c >= 0xC2 && c <= 0xDF) {
                        length = 2;
                        valid = continuation(i + 1);
                    } else if (c == 0xE0) {
                        length = 3;
                        valid = i + 2 < value.size() &&
                                static_cast<unsigned char>(value[i + 1]) >= 0xA0 &&
                                static_cast<unsigned char>(value[i + 1]) <= 0xBF &&
                                continuation(i + 2);
                    } else if ((c >= 0xE1 && c <= 0xEC) || (c >= 0xEE && c <= 0xEF)) {
                        length = 3;
                        valid = continuation(i + 1) && continuation(i + 2);
                    } else if (c == 0xED) {
                        length = 3;
                        valid = i + 2 < value.size() &&
                                static_cast<unsigned char>(value[i + 1]) >= 0x80 &&
                                static_cast<unsigned char>(value[i + 1]) <= 0x9F &&
                                continuation(i + 2);
                    } else if (c == 0xF0) {
                        length = 4;
                        valid = i + 3 < value.size() &&
                                static_cast<unsigned char>(value[i + 1]) >= 0x90 &&
                                static_cast<unsigned char>(value[i + 1]) <= 0xBF &&
                                continuation(i + 2) && continuation(i + 3);
                    } else if (c >= 0xF1 && c <= 0xF3) {
                        length = 4;
                        valid = continuation(i + 1) && continuation(i + 2) && continuation(i + 3);
                    } else if (c == 0xF4) {
                        length = 4;
                        valid = i + 3 < value.size() &&
                                static_cast<unsigned char>(value[i + 1]) >= 0x80 &&
                                static_cast<unsigned char>(value[i + 1]) <= 0x8F &&
                                continuation(i + 2) && continuation(i + 3);
                    }
                    if (!valid) {
                        out << "\\uFFFD";
                        ++i;
                    } else {
                        out.write(value.data() + static_cast<std::ptrdiff_t>(i),
                                  static_cast<std::streamsize>(length));
                        i += length;
                    }
                }
        }
    }
    return out.str();
}

std::string json_string(const std::string& value) {
    return "\"" + json_escape(value) + "\"";
}

Json make_result_document(const std::string& schema_version,
                          const std::string& command,
                          bool ok) {
    return {
        {"schema_version", schema_version},
        {"tool", "shin-context"},
        {"command", command},
        {"ok", ok}
    };
}

void emit_json_document(std::ostream& out, const Json& document) {
    out << document.dump(2) << "\n";
}

std::string json_string_array(const std::vector<std::string>& values) {
    std::ostringstream out;
    out << "[";
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i > 0) {
            out << ",";
        }
        out << json_string(values[i]);
    }
    out << "]";
    return out.str();
}

std::string json_int_array(const std::vector<int>& values) {
    if (values.empty()) {
        return "[]";
    }
    return Json(values).dump();
}

std::string json_int_map(const std::map<std::string, int>& values) {
    std::ostringstream out;
    out << "{";
    bool first = true;
    for (const auto& [key, value] : values) {
        if (!first) {
            out << ",";
        }
        first = false;
        out << json_string(key) << ":" << value;
    }
    out << "}";
    return out.str();
}

std::string slugify(const std::string& value) {
    std::string out;
    bool previous_dash = false;
    for (unsigned char raw : value) {
        char c = static_cast<char>(std::tolower(raw));
        if (std::isalnum(raw)) {
            out.push_back(c);
            previous_dash = false;
        } else if (!previous_dash && !out.empty()) {
            out.push_back('-');
            previous_dash = true;
        }
    }
    while (!out.empty() && out.back() == '-') {
        out.pop_back();
    }
    return out.empty() ? "document" : out;
}

constexpr std::uint64_t kFnv1a64Offset = 1469598103934665603ull;
constexpr std::uint64_t kFnv1a64Prime = 1099511628211ull;

static void fnv1a64_update(std::uint64_t& hash, std::string_view text) noexcept {
    for (unsigned char c : text) {
        hash ^= static_cast<std::uint64_t>(c);
        hash *= kFnv1a64Prime;
    }
}

static std::string format_fnv1a64(std::uint64_t hash) {
    std::ostringstream out;
    out << "fnv1a64:" << std::hex << std::setw(16) << std::setfill('0') << hash;
    return out.str();
}

std::string fnv1a64(std::string_view text) {
    std::uint64_t hash = kFnv1a64Offset;
    fnv1a64_update(hash, text);
    return format_fnv1a64(hash);
}

Fnv1a64Hasher::Fnv1a64Hasher() : value_(kFnv1a64Offset) {}
void Fnv1a64Hasher::update(std::string_view bytes) noexcept { fnv1a64_update(value_, bytes); }
std::string Fnv1a64Hasher::digest() const { return format_fnv1a64(value_); }

FileMetadata file_metadata(const fs::path& path) {
#ifndef _WIN32
    struct stat status {};
    if (::stat(path.c_str(), &status) != 0) {
        const std::error_code ec(errno, std::generic_category());
        throw fs::filesystem_error("Cannot read file metadata", path, ec);
    }
    if (status.st_size < 0) {
        throw fs::filesystem_error("File has a negative size", path,
                                  std::make_error_code(std::errc::io_error));
    }
#ifdef __APPLE__
    const auto& modified = status.st_mtimespec;
    const auto& changed = status.st_ctimespec;
#else
    const auto& modified = status.st_mtim;
    const auto& changed = status.st_ctim;
#endif
    auto timestamp_ticks = [](const auto& timestamp) {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::seconds(timestamp.tv_sec)).count() +
               static_cast<std::int64_t>(timestamp.tv_nsec);
    };
    return {
        static_cast<std::uintmax_t>(status.st_size),
        timestamp_ticks(modified),
        timestamp_ticks(changed),
        static_cast<std::uint64_t>(status.st_dev),
        static_cast<std::uint64_t>(status.st_ino)
    };
#else
    const windows::ReadFile input(fs::absolute(path));
    const auto value = input.stamp();
    return {value.bytes, value.modified_ns, value.changed_ns, value.volume, value.file};
#endif
}

[[noreturn]] void throw_source_observation_error(const std::string& operation,
                                                const std::error_code& error,
                                                bool missing_is_change) {
    static const std::set<std::string> allowed_operations = {
        "enumerate-source-root", "enumerate-source-entry", "read-source-metadata",
        "read-source-content", "verify-source-metadata"
    };
    const std::string safe_operation = allowed_operations.contains(operation)
                                           ? operation
                                           : "enumerate-source-entry";
    const std::error_condition condition = error.default_error_condition();
    std::string code = "source-io-error";
    std::string category = "io-error";
    std::string message = "Source filesystem observation failed.";
    std::string remediation = "Check source availability and permissions, then retry.";
    if (missing_is_change && (condition == std::errc::no_such_file_or_directory ||
                              condition == std::errc::not_a_directory)) {
        code = "file-changed-during-verification";
        category = "entry-disappeared";
        message = "A source entry disappeared during freshness verification.";
        remediation = "Retry after the concurrent writer has completed.";
    } else if (condition == std::errc::permission_denied ||
               condition == std::errc::operation_not_permitted) {
        code = "access-denied";
        category = "access-denied";
        message = "Access to a source entry was denied.";
        remediation = "Restore read access to the source entry, then retry.";
    } else if (condition == std::errc::too_many_files_open ||
               condition == std::errc::too_many_files_open_in_system ||
               condition == std::errc::not_enough_memory) {
        code = "resource-exhausted";
        category = "resource-exhausted";
        message = "Source observation could not obtain required system resources.";
        remediation = "Free system resources, then retry.";
    }
    const Json diagnostic = {
        {"schema_version", "shin.source-observation-diagnostic.v1"},
        {"stage", "source-inventory"},
        {"operation", safe_operation},
        {"category", category}
    };
    const bool retryable = code == "file-changed-during-verification";
    throw CliError(std::move(code), "filesystem", std::move(message), 1,
                   retryable, std::move(remediation), diagnostic);
}

[[noreturn]] void throw_source_observation_io_error(const std::string& operation) {
    static const std::set<std::string> allowed_operations = {
        "enumerate-source-root", "enumerate-source-entry", "read-source-metadata",
        "read-source-content", "verify-source-metadata"
    };
    const Json diagnostic = {
        {"schema_version", "shin.source-observation-diagnostic.v1"},
        {"stage", "source-inventory"},
        {"operation", allowed_operations.contains(operation) ? operation : "enumerate-source-entry"},
        {"category", "io-error"}
    };
    throw CliError("source-io-error", "filesystem", "Source filesystem observation failed.",
                   1, false, "Check source availability, then retry.", diagnostic);
}

[[noreturn]] void throw_source_changed_during_verification(const std::string& operation) {
    const Json diagnostic = {
        {"schema_version", "shin.source-observation-diagnostic.v1"},
        {"stage", "source-inventory"},
        {"operation", operation == "verify-source-metadata"
                           ? operation
                           : "enumerate-source-entry"},
        {"category", "entry-changed"}
    };
    throw CliError("file-changed-during-verification", "filesystem",
                   "A source entry changed during freshness verification.", 1, true,
                   "Retry after the concurrent writer has completed.", diagnostic);
}

[[noreturn]] void throw_file_changed_during_verification(const fs::path& path) {
    throw CliError("file-changed-during-verification", "filesystem",
                   "File changed during freshness verification: " + path.string(), 1, true,
                   "Retry after the concurrent writer has completed.");
}

FileFingerprint fingerprint_file(const fs::path& path) {
    const FileMetadata before = file_metadata(path);
    errno = 0;
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        const std::error_code ec(errno == 0 ? EIO : errno, std::generic_category());
        throw fs::filesystem_error("Cannot open source for fingerprint", path, ec);
    }
    std::array<char, 64 * 1024> buffer{};
    std::uint64_t hash = kFnv1a64Offset;
    std::uintmax_t byte_count = 0;
    while (input) {
        errno = 0;
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize count = input.gcount();
        if (count > 0) {
            fnv1a64_update(hash, std::string_view(buffer.data(), static_cast<std::size_t>(count)));
            byte_count += static_cast<std::uintmax_t>(count);
        }
    }
    if (input.bad()) {
        const std::error_code ec(errno == 0 ? EIO : errno, std::generic_category());
        throw fs::filesystem_error("Cannot read source for fingerprint", path, ec);
    }
    const FileMetadata after = file_metadata(path);
    if (before != after || byte_count != after.byte_count) {
        throw_file_changed_during_verification(path);
    }
    return {after, format_fnv1a64(hash)};
}

FileMetadata source_file_metadata(const fs::path& path, const std::string& operation) {
    try {
        return file_metadata(path);
    } catch (const CliError&) {
        throw;
    } catch (const std::system_error& error) {
        throw_source_observation_error(operation, error.code());
    }
}

FileFingerprint source_fingerprint_file(const fs::path& path, const std::string& operation) {
    try {
        return fingerprint_file(path);
    } catch (const CliError& error) {
        if (error.code() == "file-changed-during-verification") {
            throw_source_changed_during_verification("verify-source-metadata");
        }
        throw;
    } catch (const std::system_error& error) {
        throw_source_observation_error(operation, error.code());
    }
}

std::string profile_config_hash(const Profile& profile) {
    if (!profile.config_hash.empty()) {
        return profile.config_hash;
    }
    if (profile.config_source.empty()) {
        return "missing";
    }
    const fs::path path(profile.config_source);
    if (!fs::exists(path) || !fs::is_regular_file(path)) {
        return "missing";
    }
    return fnv1a64(read_file(path));
}

int markdown_heading_level(const std::string& line) {
    int level = 0;
    while (level < static_cast<int>(line.size()) && level < 6 && line[static_cast<std::size_t>(level)] == '#') {
        ++level;
    }
    if (level == 0 || level >= static_cast<int>(line.size())) {
        return 0;
    }
    return line[static_cast<std::size_t>(level)] == ' ' ? level : 0;
}

Json parse_json_text(const std::string& text, const std::string& source) {
    try {
        return Json::parse(text);
    } catch (const Json::parse_error& ex) {
        throw CliError("invalid-json", "data", "Invalid JSON in " + source + ": " + ex.what(), 1, false,
                       "Repair or regenerate the referenced JSON file before retrying.");
    }
}

bool try_parse_json_text(const std::string& text, Json& out) {
    try {
        out = Json::parse(text);
        return true;
    } catch (const Json::parse_error&) {
        out = Json();
        return false;
    }
}

Json parse_json_line(const std::string& line, const std::string& source) {
    try {
        return Json::parse(line);
    } catch (const Json::parse_error& ex) {
        throw CliError("invalid-jsonl-row", "data", "Invalid JSONL row in " + source + ": " + ex.what(),
                       1, false, "Run build/check to regenerate the referenced JSONL file.");
    }
}

const Json* find_json_field(const Json& object, const std::string& key) {
    if (!object.is_object()) {
        return nullptr;
    }
    auto it = object.find(key);
    if (it == object.end()) {
        return nullptr;
    }
    return &(*it);
}

const Json* find_json_field_any(const Json& object, const std::vector<std::string>& keys) {
    for (const std::string& key : keys) {
        const Json* value = find_json_field(object, key);
        if (value != nullptr) {
            return value;
        }
    }
    return nullptr;
}

std::string json_string_value(const Json& object, const std::string& key) {
    const Json* value = find_json_field(object, key);
    if (value == nullptr || !value->is_string()) {
        return "";
    }
    return value->get<std::string>();
}

std::string json_string_any(const Json& object, const std::vector<std::string>& keys) {
    const Json* value = find_json_field_any(object, keys);
    if (value == nullptr || !value->is_string()) {
        return "";
    }
    return value->get<std::string>();
}

int json_int_value(const Json& object, const std::string& key, int fallback) {
    const Json* value = find_json_field(object, key);
    if (value == nullptr || !value->is_number_integer()) {
        return fallback;
    }
    return value->get<int>();
}

std::vector<int> json_int_array_value(const Json& object, const std::string& key) {
    std::vector<int> values;
    const Json* array = find_json_field(object, key);
    if (array == nullptr || !array->is_array()) {
        return values;
    }
    for (const Json& item : *array) {
        if (item.is_number_integer()) {
            values.push_back(item.get<int>());
        }
    }
    return values;
}

int json_int_any(const Json& object, const std::vector<std::string>& keys, int fallback) {
    const Json* value = find_json_field_any(object, keys);
    if (value == nullptr || !value->is_number_integer()) {
        return fallback;
    }
    return value->get<int>();
}

bool json_bool_any(const Json& object, const std::vector<std::string>& keys, bool fallback) {
    const Json* value = find_json_field_any(object, keys);
    if (value == nullptr || !value->is_boolean()) {
        return fallback;
    }
    return value->get<bool>();
}

std::vector<std::string> json_string_array_value(const Json& object, const std::string& key) {
    std::vector<std::string> values;
    const Json* array = find_json_field(object, key);
    if (array == nullptr || !array->is_array()) {
        return values;
    }
    for (const Json& item : *array) {
        if (item.is_string()) {
            values.push_back(item.get<std::string>());
        }
    }
    return values;
}

std::vector<std::string> json_string_array_any(const Json& object, const std::vector<std::string>& keys) {
    const Json* array = find_json_field_any(object, keys);
    std::vector<std::string> values;
    if (array == nullptr || !array->is_array()) {
        return values;
    }
    for (const Json& item : *array) {
        if (item.is_string()) {
            values.push_back(item.get<std::string>());
        }
    }
    return values;
}

std::vector<TopicRule> extract_topic_rules(const Json& root) {
    std::vector<TopicRule> rules;
    const Json* array = find_json_field_any(root, {"topicRules", "topic_rules"});
    if (array == nullptr || !array->is_array()) {
        return rules;
    }
    for (const Json& object : *array) {
        if (!object.is_object()) {
            continue;
        }
        TopicRule rule;
        rule.id = json_string_value(object, "id");
        rule.description = json_string_value(object, "description");
        rule.keywords = json_string_array_value(object, "keywords");
        if (!rule.id.empty()) {
            rules.push_back(rule);
        }
    }
    return rules;
}

std::vector<AliasRule> extract_alias_rules(const Json& root) {
    std::vector<AliasRule> rules;
    const Json* array = find_json_field_any(root, {"aliases", "queryAliases", "query_aliases"});
    if (array == nullptr || !array->is_array()) {
        return rules;
    }
    for (const Json& object : *array) {
        if (!object.is_object()) {
            continue;
        }
        AliasRule rule;
        rule.term = json_string_any(object, {"term", "canonical", "id"});
        rule.expands = json_string_array_any(object, {"expands", "values", "aliases"});
        if (!rule.term.empty() && !rule.expands.empty()) {
            rules.push_back(rule);
        }
    }
    return rules;
}

std::vector<EvalCase> extract_eval_cases(const Json& root,
                                         std::vector<std::string>& configuration_errors) {
    std::vector<EvalCase> cases;
    const Json* array = find_json_field_any(root, {"evalCases", "goldenQueries", "golden_queries"});
    if (array == nullptr) {
        return cases;
    }
    if (!array->is_array()) {
        configuration_errors.push_back("evalCases must be an array.");
        return cases;
    }

    static const std::set<std::string> allowed_fields = {
        "id", "name", "query", "queryVariants", "query_variants",
        "mustPaths", "must_paths", "expectedPaths", "expected_paths",
        "mustTopPaths", "must_top_paths", "mustNotPaths", "must_not_paths",
        "expectedRecordTypes", "expected_record_types", "mustTopN", "must_top_n",
        "topN", "top_n", "limit", "perDocLimit", "per_doc_limit",
        "maxMatchedSections", "max_matched_sections", "maxMatchedFacts",
        "max_matched_facts", "maxCandidateRows", "max_candidate_rows",
        "minCoveredGroups", "min_covered_groups", "mustRankBefore", "must_rank_before"
    };

    std::size_t case_index = 0;
    for (const Json& object : *array) {
        const std::string location = "evalCases[" + std::to_string(case_index) + "]";
        ++case_index;
        if (!object.is_object()) {
            configuration_errors.push_back(location + " must be an object.");
            continue;
        }
        for (auto field = object.begin(); field != object.end(); ++field) {
            if (allowed_fields.find(field.key()) == allowed_fields.end()) {
                configuration_errors.push_back(location + " has unknown field: " + field.key());
            }
        }
        auto validate_field_group = [&](const std::vector<std::string>& keys,
                                        const std::string& label,
                                        const auto& validator) {
            std::size_t present = 0;
            for (const std::string& key : keys) {
                const auto value = object.find(key);
                if (value == object.end()) {
                    continue;
                }
                ++present;
                validator(key, *value);
            }
            if (present > 1) {
                configuration_errors.push_back(location + "." + label +
                                               " must use only one supported field alias.");
            }
        };
        auto validate_string = [&](const std::vector<std::string>& keys,
                                   const std::string& label) {
            validate_field_group(keys, label, [&](const std::string& key, const Json& value) {
                if (!value.is_string()) {
                    configuration_errors.push_back(location + "." + key +
                                                   " must be a string.");
                }
            });
        };
        auto validate_string_array = [&](const std::vector<std::string>& keys,
                                         const std::string& label) {
            validate_field_group(keys, label, [&](const std::string& key, const Json& value) {
                if (!value.is_array()) {
                    configuration_errors.push_back(location + "." + key +
                                                   " must be an array of strings.");
                    return;
                }
                if (!std::all_of(value.begin(), value.end(),
                                 [](const Json& item) { return item.is_string(); })) {
                    configuration_errors.push_back(location + "." + key +
                                                   " must contain only strings.");
                }
            });
        };
        auto read_integer = [&](const std::vector<std::string>& keys,
                                const std::string& label,
                                int fallback) {
            validate_field_group(keys, label, [&](const std::string& key, const Json& value) {
                if (!value.is_number_integer()) {
                    configuration_errors.push_back(location + "." + key +
                                                   " must be an integer.");
                }
            });
            const Json* value = find_json_field_any(object, keys);
            if (value == nullptr) {
                return fallback;
            }
            if (!value->is_number_integer()) {
                return fallback;
            }
            try {
                if (value->is_number_unsigned()) {
                    const unsigned long long parsed = value->get<unsigned long long>();
                    if (parsed > static_cast<unsigned long long>(std::numeric_limits<int>::max())) {
                        configuration_errors.push_back(location + "." + label +
                                                       " is outside the supported integer range.");
                        return fallback;
                    }
                    return static_cast<int>(parsed);
                }
                const long long parsed = value->get<long long>();
                if (parsed < std::numeric_limits<int>::min() ||
                    parsed > std::numeric_limits<int>::max()) {
                    configuration_errors.push_back(location + "." + label +
                                                   " is outside the supported integer range.");
                    return fallback;
                }
                return static_cast<int>(parsed);
            } catch (const std::exception&) {
                configuration_errors.push_back(location + "." + label +
                                               " is outside the supported integer range.");
                return fallback;
            }
        };

        validate_string({"id", "name"}, "id");
        validate_string({"query"}, "query");
        validate_string_array({"queryVariants", "query_variants"}, "queryVariants");
        validate_string_array({"mustPaths", "must_paths", "expectedPaths", "expected_paths"},
                              "mustPaths");
        validate_string_array({"mustTopPaths", "must_top_paths"}, "mustTopPaths");
        validate_string_array({"mustNotPaths", "must_not_paths"}, "mustNotPaths");
        validate_string_array({"expectedRecordTypes", "expected_record_types"},
                              "expectedRecordTypes");
        validate_field_group({"mustRankBefore", "must_rank_before"}, "mustRankBefore",
                             [&](const std::string& key, const Json& value) {
            if (!value.is_array()) {
                configuration_errors.push_back(location + "." + key +
                                               " must be an array of rank pair objects.");
                return;
            }
            for (std::size_t pair_index = 0; pair_index < value.size(); ++pair_index) {
                const Json& pair = value[pair_index];
                const std::string pair_location = location + "." + key + "[" +
                                                  std::to_string(pair_index) + "]";
                if (!pair.is_object()) {
                    configuration_errors.push_back(pair_location + " must be an object.");
                    continue;
                }
                for (auto field = pair.begin(); field != pair.end(); ++field) {
                    if (field.key() != "higher" && field.key() != "lower") {
                        configuration_errors.push_back(pair_location +
                                                       " has unknown field: " + field.key());
                    }
                }
                for (const char* field : {"higher", "lower"}) {
                    const Json* path = find_json_field(pair, field);
                    if (path == nullptr || !path->is_string()) {
                        configuration_errors.push_back(pair_location + "." + field +
                                                       " must be a string.");
                    }
                }
            }
        });

        EvalCase item;
        item.id = json_string_any(object, {"id", "name"});
        item.query = json_string_value(object, "query");
        item.query_variants = json_string_array_any(object, {"queryVariants", "query_variants"});
        item.must_paths = json_string_array_any(object, {"mustPaths", "must_paths", "expectedPaths", "expected_paths"});
        item.must_top_paths = json_string_array_any(object, {"mustTopPaths", "must_top_paths"});
        item.must_not_paths = json_string_array_any(object, {"mustNotPaths", "must_not_paths"});
        item.expected_record_types =
            json_string_array_any(object, {"expectedRecordTypes", "expected_record_types"});
        const Json* rank_pairs = find_json_field_any(object, {"mustRankBefore", "must_rank_before"});
        if (rank_pairs != nullptr && rank_pairs->is_array()) {
            for (const Json& pair : *rank_pairs) {
                if (!pair.is_object()) {
                    continue;
                }
                const Json* higher = find_json_field(pair, "higher");
                const Json* lower = find_json_field(pair, "lower");
                if (higher != nullptr && higher->is_string() && lower != nullptr && lower->is_string()) {
                    item.must_rank_before.push_back(
                        {higher->get<std::string>(), lower->get<std::string>()});
                }
            }
        }
        item.must_top_n = read_integer({"mustTopN", "must_top_n", "topN", "top_n"},
                                       "mustTopN", item.must_top_n);
        item.limit = read_integer({"limit"}, "limit", item.limit);
        item.per_doc_limit = read_integer({"perDocLimit", "per_doc_limit"}, "perDocLimit",
                                          item.per_doc_limit);
        item.max_matched_sections = read_integer(
            {"maxMatchedSections", "max_matched_sections"}, "maxMatchedSections",
            item.max_matched_sections);
        item.max_matched_facts = read_integer(
            {"maxMatchedFacts", "max_matched_facts"}, "maxMatchedFacts",
            item.max_matched_facts);
        item.max_candidate_rows = read_integer(
            {"maxCandidateRows", "max_candidate_rows"}, "maxCandidateRows",
            item.max_candidate_rows);
        item.min_covered_groups = read_integer(
            {"minCoveredGroups", "min_covered_groups"}, "minCoveredGroups",
            item.min_covered_groups);
        const bool has_assertions = !item.must_paths.empty() || !item.must_top_paths.empty() ||
                                    !item.must_not_paths.empty() || !item.expected_record_types.empty() ||
                                    item.max_matched_sections >= 0 || item.max_matched_facts >= 0 ||
                                    item.max_candidate_rows >= 0 || item.min_covered_groups >= 0 ||
                                    !item.must_rank_before.empty();
        bool structurally_valid = true;
        if (item.id.empty()) {
            configuration_errors.push_back(location + ".id must be a non-empty string.");
            structurally_valid = false;
        }
        if (item.query.empty()) {
            configuration_errors.push_back(location + ".query must be a non-empty string.");
            structurally_valid = false;
        }
        if (!has_assertions) {
            configuration_errors.push_back(location +
                                           " must define at least one retrieval assertion.");
            structurally_valid = false;
        }
        if (structurally_valid) {
            cases.push_back(item);
        }
    }
    return cases;
}

std::string title_from_path(const std::string& path) {
    fs::path p(path);
    std::string stem = p.stem().string();
    std::replace(stem.begin(), stem.end(), '-', ' ');
    std::replace(stem.begin(), stem.end(), '_', ' ');
    if (!stem.empty()) {
        stem[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(stem[0])));
    }
    return stem;
}

std::string extension_of(const std::string& path) {
    return to_lower(fs::path(path).extension().string());
}

bool extension_allowed(const std::vector<std::string>& extensions, const std::string& path) {
    const std::string ext = extension_of(path);
    return std::find(extensions.begin(), extensions.end(), ext) != extensions.end();
}

std::string basename_of(const std::string& path) {
    return to_lower(fs::path(path).filename().string());
}

bool contains_any(const std::string& lower_text, const std::vector<std::string>& terms) {
    for (const std::string& term : terms) {
        if (lower_text.find(term) != std::string::npos) {
            return true;
        }
    }
    return false;
}

void append_unique_string(std::vector<std::string>& values, const std::string& value) {
    if (!value.empty() && std::find(values.begin(), values.end(), value) == values.end()) {
        values.push_back(value);
    }
}

static bool is_secret_token_char(unsigned char c) {
    return std::isalnum(c) != 0 || c == '-' || c == '_' || c == '.' || c == '/' || c == '+' || c == '=';
}

static std::string trim_secret_token(std::string token) {
    while (!token.empty() && !is_secret_token_char(static_cast<unsigned char>(token.front()))) {
        token.erase(token.begin());
    }
    while (!token.empty() && !is_secret_token_char(static_cast<unsigned char>(token.back()))) {
        token.pop_back();
    }
    return token;
}

static std::vector<std::string> secret_candidate_tokens(const std::string& text) {
    std::vector<std::string> tokens;
    std::string token;
    auto flush = [&]() {
        std::string trimmed = trim_secret_token(token);
        if (!trimmed.empty()) {
            tokens.push_back(trimmed);
        }
        token.clear();
    };
    for (unsigned char c : text) {
        if (is_secret_token_char(c)) {
            token.push_back(static_cast<char>(c));
        } else {
            flush();
        }
    }
    flush();
    return tokens;
}

static bool is_placeholder_secret_text(const std::string& value) {
    const std::string lower = to_lower(value);
    if (contains_any(lower,
                     {"example", "placeholder", "redacted", "dummy", "changeme", "change-me",
                      "sample", "fake", "test-only", "fixture", "baseline", "probe", "reset",
                      "must-not", "not-ship", "do-not", "secret-token", "generated", "throwaway",
                      "kubernetes-secret", "from-kubernetes-secret", "closed-until", "your-",
                      "legacy-", "xxx", "...", "<", ">"})) {
        return true;
    }
    return lower.ends_with("-secret") &&
           (starts_with(lower, "local-development-") ||
            contains_any(lower, {"client-", "inline-", "test-", "fake-", "dummy-", "example-"}));
}

static bool is_alnum_token(const std::string& token) {
    if (token.empty()) {
        return false;
    }
    for (unsigned char c : token) {
        if (std::isalnum(c) == 0) {
            return false;
        }
    }
    return true;
}

static int alnum_count(const std::string& value) {
    int count = 0;
    for (unsigned char c : value) {
        if (std::isalnum(c) != 0) {
            ++count;
        }
    }
    return count;
}

static bool looks_like_jwt_token(const std::string& token) {
    if (token.size() < 40 || !starts_with(token, "eyJ") || is_placeholder_secret_text(token)) {
        return false;
    }
    int dots = 0;
    for (unsigned char c : token) {
        if (c == '.') {
            ++dots;
            continue;
        }
        if (std::isalnum(c) == 0 && c != '-' && c != '_') {
            return false;
        }
    }
    return dots >= 2;
}

static std::size_t find_unquoted_assignment_separator(const std::string& text) {
    bool in_single_quote = false;
    bool in_double_quote = false;
    bool escaped = false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (escaped) {
            escaped = false;
            continue;
        }
        if (c == '\\' && (in_single_quote || in_double_quote)) {
            escaped = true;
            continue;
        }
        if (c == '\'' && !in_double_quote) {
            in_single_quote = !in_single_quote;
            continue;
        }
        if (c == '"' && !in_single_quote) {
            in_double_quote = !in_double_quote;
            continue;
        }
        if (in_single_quote || in_double_quote) {
            continue;
        }
        if (c == '=' && (i == 0 || (text[i - 1] != '!' && text[i - 1] != '<' &&
                                    text[i - 1] != '>' && text[i - 1] != '=')) &&
            (i + 1 >= text.size() || (text[i + 1] != '=' && text[i + 1] != '>'))) {
            return i;
        }
        if (c == ':' && (i == 0 || text[i - 1] != ':') &&
            (i + 1 >= text.size() || text[i + 1] != ':') &&
            (i + 2 >= text.size() || text[i + 1] != '/' || text[i + 2] != '/')) {
            return i;
        }
    }
    return std::string::npos;
}

static bool credential_key_side_is_sensitive(const std::string& key_side) {
    const std::vector<std::string> tokens = secret_candidate_tokens(key_side);
    if (tokens.empty() || tokens.size() > 4) {
        return false;
    }
    const std::string key = to_lower(tokens.back());
    for (const char* suffix : {"api_key", "apikey", "secret", "password", "token", "access_key", "private_key"}) {
        if (key == suffix || key.ends_with(suffix) || key.ends_with(std::string(suffix) + "value")) {
            return true;
        }
    }
    return false;
}

static bool looks_like_assigned_secret_value(const std::string& token) {
    if (token.size() < 16 || alnum_count(token) < 12 || is_placeholder_secret_text(token)) {
        return false;
    }
    const std::string lower = to_lower(token);
    if (starts_with(lower, "projects/") && lower.find("/secrets/") != std::string::npos) {
        return false;
    }
    int digits = 0;
    int alpha = 0;
    int special = 0;
    for (unsigned char c : token) {
        if (std::isdigit(c) != 0) {
            ++digits;
        } else if (std::isalpha(c) != 0) {
            ++alpha;
        } else if (c != '_') {
            ++special;
        }
    }
    if (alpha == 0) {
        return false;
    }
    if (digits > 0) {
        if (special == 0) {
            return token.size() >= 24;
        }
        return true;
    }
    if (token.find('.') != std::string::npos) {
        return false;
    }
    return special > 0 && token.size() >= 20;
}

static std::string assigned_value_text(const std::string& input) {
    // A complete call computes a value; its arguments are not the assigned literal.
    // Unknown expressions stay conservative. Signature rules still scan all input.
    std::string_view value(input);
    std::size_t groups = 0;
    while (value.starts_with('(')) {
        ++groups;
        value.remove_prefix(1);
        const auto start = value.find_first_not_of(" \t\r\n");
        value.remove_prefix(start == std::string_view::npos ? value.size() : start);
    }
    for (const std::string_view prefix : {"await ", "new "})
        if (value.starts_with(prefix)) value.remove_prefix(prefix.size());
    const auto open = value.find('(');
    if (open == std::string_view::npos || open == 0) return input;
    const auto callee = trim(std::string(value.substr(0, open)));
    if (callee.empty() || !std::all_of(callee.begin(), callee.end(), [](unsigned char c) {
            return std::isalnum(c) != 0 || c == '_' || c == '.' || c == '$' || c == ':';
        })) return input;

    std::size_t depth = 1;
    char quote = 0;
    bool escaped = false;
    for (std::size_t pos = open + 1; pos < value.size(); ++pos) {
        const char c = value[pos];
        if (escaped) { escaped = false; continue; }
        if (quote != 0) {
            if (c == '\\') escaped = true;
            else if (c == quote) quote = 0;
            continue;
        }
        if (c == '\'' || c == '"' || c == '`') { quote = c; continue; }
        if (c == '(') ++depth;
        if (c != ')' || --depth != 0) continue;
        auto tail = value.substr(pos + 1);
        const auto skip_space = [&] {
            const auto next = tail.find_first_not_of(" \t\r\n");
            tail.remove_prefix(next == std::string_view::npos ? tail.size() : next);
        };
        skip_space();
        while (groups > 0 && tail.starts_with(')')) {
            --groups;
            tail.remove_prefix(1);
            skip_space();
        }
        if (groups != 0) return input;
        if (tail.starts_with(';')) { tail.remove_prefix(1); skip_space(); }
        return tail.empty() || tail.starts_with("//") || tail.starts_with('#') ? std::string{} : input;
    }
    return input;
}

static std::string assignment_candidate_text(const std::string& input) {
    // A standalone typed arrow-function header declares code, not a secret.
    // Do not exempt defaults, literals, expression bodies or trailing statements.
    auto header = trim(input);
    if (starts_with(header, "async ")) header = trim(header.substr(6));
    const auto arrow = header.find("=>");
    if (header.starts_with('(') && arrow != std::string::npos &&
        trim(header.substr(arrow + 2)) == "{") {
        const auto signature = header.substr(0, arrow);
        int depth = 0;
        bool closed = false;
        bool valid = true;
        for (const unsigned char c : signature) {
            if (!(std::isalnum(c) || std::isspace(c) ||
                  (c < 128 && std::string_view("_$.,:?![]<>|&()").find(static_cast<char>(c)) != std::string_view::npos))) {
                valid = false;
                break;
            }
            if (c == '(') ++depth;
            if (c == ')') {
                if (--depth < 0) { valid = false; break; }
                if (depth == 0) closed = true;
            }
        }
        if (valid && closed && depth == 0) return {};
    }
    const auto value = assigned_value_text(input);
    // In composed expressions a callee name is code, not a credential value.
    // Keep every argument/literal and all signature scanning. Malformed input
    // retains the original conservative scan rather than guessing call boundaries.
    std::size_t depth = 0;
    char quote = 0;
    bool escaped = false;
    for (const char c : value) {
        if (escaped) { escaped = false; continue; }
        if (quote) {
            if (c == '\\') escaped = true;
            else if (c == quote) quote = 0;
        } else if (c == '\'' || c == '"' || c == '`') quote = c;
        else if (c == '(') ++depth;
        else if (c == ')') {
            if (!depth) return value;
            --depth;
        }
    }
    if (quote || depth) return value;
    std::string result;
    for (std::size_t pos = 0; pos < value.size();) {
        const char c = value[pos];
        if (quote) {
            result += c;
            ++pos;
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == quote) quote = 0;
            continue;
        }
        if (c == '\'' || c == '"' || c == '`') {
            quote = c;
            result += c;
            ++pos;
            continue;
        }
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_' || c == '$') {
            const auto start = pos++;
            while (pos < value.size()) {
                const auto next = static_cast<unsigned char>(value[pos]);
                if (!std::isalnum(next) && next != '_' && next != '$' && next != '.' && next != ':') break;
                ++pos;
            }
            auto after = pos;
            while (after < value.size() && std::isspace(static_cast<unsigned char>(value[after]))) ++after;
            if (after < value.size() && value[after] == '(') result += ' ';
            else result.append(value, start, pos - start);
        } else {
            result += c;
            ++pos;
        }
    }
    return result;
}

static std::size_t find_ascii_insensitive(const std::string& text, std::string_view needle) {
    const auto found = std::search(text.begin(), text.end(), needle.begin(), needle.end(),
        [](unsigned char value, unsigned char expected) {
            return (value >= 'A' && value <= 'Z' ? value + ('a' - 'A') : value) == expected;
        });
    return found == text.end() ? std::string::npos : static_cast<std::size_t>(found - text.begin());
}

SecretScanHit detect_secret_like_text(const std::string& text, bool allow_source_suppression) {
    if (allow_source_suppression && find_ascii_insensitive(text, "gitleaks:allow") != std::string::npos) {
        return {};
    }
    const std::string private_key_head = std::string("-----") + "BEGIN ";
    const std::string private_key_tail = std::string(" PRIVATE ") + "KEY-----";
    const std::size_t private_key_begin = text.find(private_key_head);
    const std::size_t private_key_end = text.find(private_key_tail);
    if (private_key_begin != std::string::npos && private_key_end != std::string::npos &&
        private_key_end > private_key_begin + private_key_head.size()) {
        const std::string key_type =
            text.substr(private_key_begin + private_key_head.size(),
                        private_key_end - private_key_begin - private_key_head.size());
        const bool literal_key_type = std::all_of(key_type.begin(), key_type.end(), [](unsigned char c) {
            return std::isalnum(c) != 0 || c == ' ' || c == '-';
        });
        if (literal_key_type) {
            return {true, "private-key-block"};
        }
    }

    // These signature rules all require one of these exact byte prefixes.
    // Avoid allocating every word of ordinary files during whole-file scans.
    if (contains_any(text, {"AKIA", "ASIA", "sk-", "eyJ"})) {
        for (const std::string& token : secret_candidate_tokens(text)) {
            if ((starts_with(token, "AKIA") || starts_with(token, "ASIA")) && token.size() == 20 &&
                is_alnum_token(token) && !is_placeholder_secret_text(token)) {
                return {true, "aws-access-key-id"};
            }
            if (starts_with(token, "sk-") && token.size() >= 24 && !is_placeholder_secret_text(token)) {
                return {true, "openai-api-key"};
            }
            if (looks_like_jwt_token(token)) {
                return {true, "jwt-token"};
            }
        }
    }

    const std::size_t bearer = find_ascii_insensitive(text, "bearer ");
    if (bearer != std::string::npos) {
        std::string after = text.substr(bearer + 7);
        after.erase(0, after.find_first_not_of(" \t"));
        if (!after.empty() && after.front() != '"' && after.front() != '\'' &&
            after.front() != '<' && after.front() != '[' && after.front() != '{') {
            for (const std::string& token : secret_candidate_tokens(after)) {
                if (token.size() >= 20 && alnum_count(token) >= 16 && !is_placeholder_secret_text(token)) {
                    return {true, "bearer-token"};
                }
                break;
            }
        }
    }

    const std::size_t sep = find_unquoted_assignment_separator(text);
    if (sep != std::string::npos) {
        const std::string key_side = text.substr(0, sep);
        if (key_side.size() <= 128 && credential_key_side_is_sensitive(key_side)) {
            const std::string value_side = text.substr(sep + 1);
            const std::string compact_value = trim(value_side);
            if (!compact_value.empty() && compact_value.front() == '<' &&
                compact_value.find('>') != std::string::npos) {
                return {};
            }
            for (const std::string& token : secret_candidate_tokens(assignment_candidate_text(compact_value))) {
                if (looks_like_assigned_secret_value(token)) {
                    return {true, "credential-assignment"};
                }
            }
        }
    }

    return {};
}

std::string redact_sensitive_text(const std::string& text) {
    const SecretScanHit hit = detect_secret_like_text(text);
    if (!hit.found) {
        return text;
    }
    return "[REDACTED:" + hit.kind + "]";
}

static bool path_has_numeric_decision_name(const std::string& lower_name) {
    const std::size_t separator = lower_name.find('-');
    return separator != std::string::npos && separator > 0 && separator + 1 < lower_name.size() &&
           std::all_of(lower_name.begin(), lower_name.begin() + static_cast<std::ptrdiff_t>(separator),
                       [](unsigned char value) { return is_ascii_digit(value); });
}

std::string detect_area(const std::string& path) {
    const std::string lower = to_lower(path);
    constexpr std::string_view docs_prefix = "docs/";
    if (!starts_with(lower, docs_prefix)) {
        return "root";
    }
    const std::size_t area_start = docs_prefix.size();
    const std::size_t separator = lower.find('/', area_start);
    if (separator == std::string::npos) {
        return "docs";
    }
    return lower.substr(area_start, separator - area_start);
}

std::string detect_doc_type(const std::string& path,
                            const std::string& format,
                            const std::string& title,
                            const std::string& content) {
    const std::string lower_path = to_lower(path);
    const std::string lower_name = basename_of(path);
    const std::string lower_title = to_lower(title);
    const std::string head = to_lower(content.substr(0, std::min<std::size_t>(content.size(), 4096)));
    if (format == "jsonl") {
        if (lower_path.find("/facts/") != std::string::npos || lower_path.find("facts/") == 0) {
            return "fact-store";
        }
        if (lower_name.find("findings") != std::string::npos) {
            return "finding-store";
        }
        return "jsonl-artifact";
    }
    if (format == "json") {
        if (lower_name.find("schema") != std::string::npos || lower_title.find("schema") != std::string::npos) {
            return "schema";
        }
        if (lower_path.find("openapi") != std::string::npos) {
            return "openapi-contract";
        }
        if (lower_path.find("asyncapi") != std::string::npos) {
            return "asyncapi-contract";
        }
        if (lower_path.find("manifest") != std::string::npos || head.find("\"schema_version\"") != std::string::npos ||
            head.find("\"schemaversion\"") != std::string::npos) {
            return "manifest";
        }
        return "json-artifact";
    }
    if (lower_path.find("/decisions/") != std::string::npos && path_has_numeric_decision_name(lower_name)) {
        return "adr";
    }
    if (lower_name == "decisions-index.md" || lower_name == "service-decisions-index.md") {
        return "decision-index";
    }
    if (lower_name.find("runbook") != std::string::npos ||
        lower_title.find("runbook") != std::string::npos ||
        lower_path.find("/operations/") != std::string::npos ||
        lower_name.find("troubleshooting") != std::string::npos) {
        return "runbook";
    }
    if (contains_any(lower_name + "\n" + lower_title,
                     {"contract", "capability", "schema", "openapi", "asyncapi", "api"})) {
        return "contract";
    }
    if (lower_name.find("security") != std::string::npos) {
        return "security";
    }
    if (lower_name.find("deployment") != std::string::npos || lower_name.find("kubernetes") != std::string::npos ||
        lower_name.find("docker") != std::string::npos) {
        return "deployment";
    }
    if (lower_name.find("architecture") != std::string::npos || lower_name.find("boundary") != std::string::npos ||
        lower_name.find("mental") != std::string::npos) {
        return "architecture";
    }
    if (lower_name.find("todo") != std::string::npos || lower_name.find("open-questions") != std::string::npos ||
        lower_name.find("assumptions") != std::string::npos || lower_name.find("plan") != std::string::npos) {
        return "planning";
    }
    if (lower_name.find("handoff") != std::string::npos) {
        return "handoff";
    }
    if (lower_name.find("context") != std::string::npos) {
        return "context";
    }
    if (lower_name.find("progress") != std::string::npos || lower_path.find("/progress/") != std::string::npos) {
        return "history";
    }
    if (lower_name == "readme.md" || lower_name == "index.md") {
        return "index";
    }
    return "reference";
}

int first_heading_line(const std::vector<std::string>& lines) {
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (markdown_heading_level(lines[i]) > 0) {
            return static_cast<int>(i + 1);
        }
    }
    return 1;
}

std::string detect_status(const std::string& path, const std::string& content, const Json* json_doc) {
    const std::string metadata_status = explicit_metadata_status(json_doc);
    if (!metadata_status.empty()) {
        return metadata_status;
    }
    const std::string document_status = explicit_document_status(content);
    return document_status.empty() ? fallback_status_from_path(path) : document_status;
}

std::string preview_from_lines(const std::vector<std::string>& lines,
                               std::size_t start,
                               int stop_markdown_level,
                               std::size_t limit) {
    std::string preview;
    for (std::size_t i = start; i < lines.size(); ++i) {
        if (stop_markdown_level > 0) {
            int level = markdown_heading_level(lines[i]);
            if (level > 0 && level <= stop_markdown_level) {
                break;
            }
        }
        std::string line = redact_sensitive_text(compact_whitespace(lines[i]));
        if (line.empty()) {
            continue;
        }
        if (!preview.empty()) {
            preview.push_back(' ');
        }
        preview += line;
        if (preview.size() >= limit) {
            break;
        }
    }
    return truncate_text(redact_sensitive_text(preview), limit);
}

std::string preview_from_line_range(const std::vector<std::string>& lines,
                                    std::size_t start,
                                    std::size_t end,
                                    std::size_t limit) {
    std::string preview;
    const std::size_t capped_end = std::min(end, lines.size());
    for (std::size_t i = start; i < capped_end; ++i) {
        std::string line = redact_sensitive_text(compact_whitespace(lines[i]));
        if (line.empty()) {
            continue;
        }
        if (!preview.empty()) {
            preview.push_back(' ');
        }
        preview += line;
        if (preview.size() >= limit) {
            break;
        }
    }
    return truncate_text(redact_sensitive_text(preview), limit);
}

std::string extract_json_key_line(const std::string& line) {
    std::size_t i = 0;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) {
        ++i;
    }
    if (i >= line.size() || line[i] != '"') {
        return "";
    }
    std::string key;
    bool escaped = false;
    for (++i; i < line.size(); ++i) {
        char c = line[i];
        if (escaped) {
            key.push_back(c);
            escaped = false;
            continue;
        }
        if (c == '\\') {
            escaped = true;
            continue;
        }
        if (c == '"') {
            ++i;
            break;
        }
        key.push_back(c);
    }
    while (i < line.size() && line[i] == ' ') {
        ++i;
    }
    return i < line.size() && line[i] == ':' ? key : "";
}

} // namespace shin::context
