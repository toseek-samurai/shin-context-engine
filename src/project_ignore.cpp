#include "project_ignore.h"
#include "context_internal.h"
#include "execution_io.h"
#include <re2/set.h>
#include <simdjson/simdjson.h>

namespace shin::context {
namespace {

constexpr std::size_t kMaxShinIgnoreBytes = 32 * 1024;
constexpr std::size_t kMaxGitIgnoreBytes = 1024 * 1024;

[[noreturn]] void invalid_rules(const std::string& path, const std::string& detail) {
    throw CliError("invalid-project-exclusions", "configuration", "Invalid project rules in " + path + ". " + detail,
                   1, false, "Review the indicated ignore file and retry.");
}

std::string read_rules(const fs::path& root, const std::string& path, bool shin) {
    std::error_code ec;
    const auto state = fs::symlink_status(root / path, ec);
    if (ec == std::errc::no_such_file_or_directory || ec == std::errc::not_a_directory ||
        (!ec && !fs::exists(state))) return {};
    if (ec) throw_source_observation_error("read-project-exclusions", ec);
    if (!shin && fs::is_symlink(state)) return {}; // Git never follows a .gitignore symlink.
    if (!fs::is_regular_file(state))
        throw CliError("unsafe-project-exclusions", "configuration", path + " must be a regular file.",
                       1, false, "Review the indicated project rules file.");
    try {
        return read_bounded_source_file(root, path, shin ? kMaxShinIgnoreBytes : kMaxGitIgnoreBytes);
    } catch (const CliError& error) {
        if (error.code() != "execution-input-too-large") throw;
        invalid_rules(path, shin ? "File exceeds 32 KiB." : "File exceeds 1 MiB.");
    }
}

std::string git_file(std::string_view directory) {
    return directory.empty() ? ".gitignore" : std::string(directory) + "/.gitignore";
}

Fnv1a64Hasher scope_hash(const std::string& shin_digest) {
    Fnv1a64Hasher hash;
    hash.update("project-ignore-v2\n");
    hash.update(shin_digest);
    return hash;
}

void hash_directory(Fnv1a64Hasher& hash, const std::string& dir, const std::string& digest) {
    // New directories without rules are source membership changes only.
    // Retaining their locations separately lets us detect a later .gitignore.
    if (digest.empty()) return;
    hash.update("\n");
    hash.update(dir);
    hash.update(std::string_view("\0", 1));
    hash.update(digest);
}

re2::RE2::Options glob_options() {
    re2::RE2::Options options;
    options.set_encoding(re2::RE2::Options::EncodingLatin1);
    options.set_log_errors(false);
    return options;
}

std::string hex_byte(unsigned char c) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result = "\\x00";
    result[2] = digits[c >> 4];
    result[3] = digits[c & 15];
    return result;
}

// Convert one Git bracket expression to a byte class that can never consume a slash.
// RE2 handles ranges and POSIX classes; escaped characters in a glob are literals.
bool append_class(std::string_view glob, std::size_t& cursor, std::string& expression) {
    std::string cls = "[";
    auto i = cursor + 1;
    if (i < glob.size() && (glob[i] == '!' || glob[i] == '^')) { cls += '^'; ++i; }
    if (i < glob.size() && glob[i] == ']') { cls += "\\x5d"; ++i; }
    for (; i < glob.size() && glob[i] != ']'; ++i) {
        if (glob[i] == '/' || (glob[i] == '\\' && i + 1 == glob.size())) return false;
        if (glob[i] == '\\') cls += hex_byte(static_cast<unsigned char>(glob[++i]));
        else if (glob.substr(i, 2) == "[:") {
            const auto close = glob.find(":]", i + 2);
            if (close == std::string_view::npos) return false;
            cls += glob.substr(i, close + 2 - i);
            i = close + 1;
        } else cls += glob[i];
    }
    if (i == glob.size()) return false;
    const re2::RE2 predicate(cls + "]", glob_options());
    if (!predicate.ok()) return false;
    std::string accepted;
    for (unsigned byte = 1; byte < 256; ++byte) {
        const auto matches = [&](unsigned value) {
            const char candidate = static_cast<char>(value);
            return candidate != '/' && candidate != '\n' && re2::RE2::FullMatch(std::string_view(&candidate, 1), predicate);
        };
        if (!matches(byte)) continue;
        const auto start = byte;
        while (byte < 255 && matches(byte + 1)) ++byte;
        accepted += hex_byte(static_cast<unsigned char>(start));
        if (byte != start) accepted += "-" + hex_byte(static_cast<unsigned char>(byte));
    }
    if (accepted.empty()) return false;
    expression += "[" + accepted + "]";
    cursor = i + 1;
    return true;
}

std::optional<std::string> glob_expression(std::string_view pattern, bool anchored, bool directory_only) {
    std::string out = anchored ? "" : "(?:.*/)?";
    for (std::size_t i = 0; i < pattern.size();) {
        const char c = pattern[i];
        if (c == '*') {
            const auto start = i;
            while (i < pattern.size() && pattern[i] == '*') ++i;
            const bool globstar = i - start >= 2 && (start == 0 || pattern[start - 1] == '/');
            if (globstar && i == pattern.size()) out += ".*";
            else if (globstar && pattern[i] == '/') { out += "(?:.*/)?"; ++i; }
            else out += "[^/\\n]*";
        } else if (c == '?') { out += "[^/\\n]"; ++i; }
        else if (c == '[') {
            if (!append_class(pattern, i, out)) return std::nullopt;
        } else {
            if (c == '\\' && ++i == pattern.size()) return std::nullopt;
            const auto start = i++;
            if (c != '\\') {
                const auto next = pattern.find_first_of("*?[\\", i);
                i = next == std::string_view::npos ? pattern.size() : next;
            }
            out += re2::RE2::QuoteMeta(pattern.substr(start, i - start));
        }
    }
    out += directory_only ? "\\n" : "\\n?";
    return out;
}

void trim_git_spaces(std::string& rule) {
    while (!rule.empty() && rule.back() == ' ') {
        auto i = rule.size() - 1;
        while (i && rule[i - 1] == '\\') --i;
        if ((rule.size() - 1 - i) % 2) break;
        rule.pop_back();
    }
}

bool path_within(std::string_view path, std::string_view directory) {
    return directory.empty() || path == directory ||
           (path.size() > directory.size() && path.starts_with(directory) && path[directory.size()] == '/');
}

} // namespace

class IgnoreRules {
public:
    IgnoreRules(const std::string& path, const std::string& bytes, bool shin)
        : matcher_(glob_options(), re2::RE2::ANCHOR_BOTH) {
        if (shin && (!simdjson::validate_utf8(bytes.data(), bytes.size()) ||
            std::any_of(bytes.begin(), bytes.end(), [](unsigned char c) {
                return c < 0x20 && c != '\t' && c != '\n' && c != '\r' && c != '\f';
            }))) invalid_rules(path, "Expected UTF-8 text.");
        std::istringstream input(bytes);
        std::size_t line = 0;
        for (std::string rule; std::getline(input, rule);) {
            ++line;
            if (line == 1 && rule.starts_with("\xEF\xBB\xBF")) rule.erase(0, 3);
            if (rule.ends_with('\r')) rule.pop_back();
            if (shin) rule = trim(rule);
            else trim_git_spaces(rule);
            if (rule.empty() || rule.front() == '#') continue;
            const auto invalid = [&] { invalid_rules(path, "Check line " + std::to_string(line) + "."); };
            if (shin && (includes_.size() >= 256 || rule.size() > 1024)) invalid();
            const bool include = rule.front() == '!';
            if (include) rule.erase(0, 1);
            const bool root_anchor = rule.starts_with('/');
            if (root_anchor) rule.erase(0, 1);
            const bool dir_only = rule.ends_with('/');
            if (dir_only) rule.pop_back();
            if (shin && (!profile_relative_path_valid(rule) || rule.find("//") != std::string::npos ||
                         rule.find(':') != std::string::npos)) invalid();
            if (rule.empty()) continue;
            const bool anchored = root_anchor || rule.find('/') != std::string::npos;
            const auto expression = glob_expression(rule, anchored, dir_only);
            if (!expression) {
                if (shin) invalid();
                continue; // A malformed Git pattern matches nothing.
            }
            if (matcher_.Add(*expression, nullptr) < 0) invalid();
            if (shin && include) {
                // Only descend into ignored trees that this inclusion could reach.
                std::string prefix;
                if (anchored) {
                    const auto wildcard = rule.find_first_of("*?[\\");
                    prefix = rule.substr(0, wildcard);
                    if (wildcard != std::string::npos) {
                        const auto slash = prefix.rfind('/');
                        prefix = slash == std::string::npos ? "" : prefix.substr(0, slash);
                    }
                }
                inclusion_prefixes_.emplace_back(static_cast<int>(includes_.size()), std::move(prefix));
            }
            includes_.push_back(include);
        }
        if (!includes_.empty() && !matcher_.Compile()) invalid_rules(path, "Patterns exceed the matcher budget.");
    }

    int last_match(std::string_view path) const {
        if (includes_.empty()) return -1;
        std::vector<int> hits;
        re2::RE2::Set::ErrorInfo info{};
        if (matcher_.Match(path, &hits, &info)) return *std::max_element(hits.begin(), hits.end());
        if (info.kind != re2::RE2::Set::kNoError)
            throw CliError("invalid-project-exclusions", "configuration", "Project rules exceed the matcher budget.",
                           1, false, "Review the active ignore patterns.");
        return -1;
    }
    bool includes(int index) const { return includes_[static_cast<std::size_t>(index)]; }
    bool may_include_below(const std::string& directory, int after) const {
        return std::any_of(inclusion_prefixes_.begin(), inclusion_prefixes_.end(), [&](const auto& entry) {
            return entry.first > after && (path_within(directory, entry.second) || path_within(entry.second, directory));
        });
    }
private:
    re2::RE2::Set matcher_;
    std::vector<bool> includes_;
    std::vector<std::pair<int, std::string>> inclusion_prefixes_;
};

namespace {
std::shared_ptr<const IgnoreRules> rules_for(const std::string& path, const std::string& bytes, bool shin) {
    if (bytes.empty()) return {};
    // Reuse compiled patterns across events and scans. Always read the current
    // file before consulting this cache. Bound both entries and retained input.
    struct Cached { std::string bytes; bool shin; std::shared_ptr<const IgnoreRules> rules; };
    thread_local std::vector<Cached> cache;
    thread_local std::size_t retained = 0;
    for (const auto& entry : cache)
        if (entry.shin == shin && entry.bytes == bytes) return entry.rules;
    auto rules = std::make_shared<IgnoreRules>(path, bytes, shin);
    if (cache.size() >= 32 || retained + bytes.size() > kMaxGitIgnoreBytes) { cache.clear(); retained = 0; }
    cache.push_back({bytes, shin, rules});
    retained += bytes.size();
    return rules;
}
} // namespace

ProjectExclusions::ProjectExclusions(const fs::path& root) : root_(root) {
    const auto bytes = read_rules(root, ".shinignore", true);
    shin_digest_ = fnv1a64(bytes);
    shin_ = rules_for(".shinignore", bytes, true);
    enter_directory("");
}

void ProjectExclusions::enter_directory(const std::string& dir) {
    const auto path = git_file(dir);
    const auto bytes = read_rules(root_, path, false);
    git_digests_.emplace(dir, bytes.empty() ? "" : fnv1a64(bytes));
    if (!bytes.empty()) git_rules_.emplace(dir, rules_for(path, bytes, false));
}

ProjectExclusions::Decision ProjectExclusions::decide(const std::string& path, bool directory, Decision parent) const {
    // Newline is forbidden in eligible paths. A separate directory marker keeps
    // patterns such as dir/* from accidentally excluding dir itself.
    const auto subject = directory ? path + '\n' : path;
    if (!parent.git_ignored && !git_rules_.empty()) {
        std::string_view base(path);
        for (;;) {
            const auto slash = base.rfind('/');
            base = slash == std::string_view::npos ? std::string_view{} : base.substr(0, slash);
            if (const auto rules = git_rules_.find(base); rules != git_rules_.end()) {
                const auto local = std::string_view(subject).substr(base.empty() ? 0 : base.size() + 1);
                const auto match = rules->second->last_match(local);
                if (match >= 0) { parent.git_ignored = !rules->second->includes(match); break; }
            }
            if (base.empty()) break;
        }
    }
    if (shin_) parent.shin_rule = std::max(parent.shin_rule, shin_->last_match(subject));
    parent.excluded = parent.shin_rule < 0 ? parent.git_ignored : !shin_->includes(parent.shin_rule);
    return parent;
}

bool ProjectExclusions::may_include_below(const std::string& directory, Decision decision) const {
    return shin_ && shin_->may_include_below(directory, decision.shin_rule);
}

ProjectIgnoreBinding ProjectExclusions::binding() const {
    auto hash = scope_hash(shin_digest_);
    ProjectIgnoreBinding result;
    for (const auto& [dir, digest] : git_digests_) {
        hash_directory(hash, dir, digest);
        if (!dir.empty()) result.directories.push_back(dir);
    }
    result.revision = hash.digest();
    return result;
}

bool ProjectIgnoreBinding::current(const fs::path& root,
    const std::function<void(const std::string&, const std::string&)>& observe) const {
    if (revision.empty()) return true;
    const auto read = [&](const std::string& path, bool shin) {
        auto bytes = read_rules(root, path, shin);
        if (observe && !bytes.empty()) observe(path, bytes);
        return bytes;
    };
    auto hash = scope_hash(fnv1a64(read(".shinignore", true)));
    const auto collect = [&](const std::string& dir) {
        const auto bytes = read(git_file(dir), false);
        hash_directory(hash, dir, bytes.empty() ? "" : fnv1a64(bytes));
    };
    collect("");
    for (const auto& directory : directories) collect(directory);
    return hash.digest() == revision;
}

ProjectIgnoreBinding ProjectIgnoreBinding::from_summary(const Json& summary) {
    const auto fail = [] { throw_index_error("project-exclusions-changed", "Missing or invalid project ignore binding."); };
    if (!summary.is_object()) fail();
    ProjectIgnoreBinding result;
    result.revision = json_string_value(summary, "exclusion_revision");
    const auto* directories = find_json_field(summary, "git_ignore_directories");
    if (result.revision.empty() || !directories || !directories->is_array()) fail();
    for (const auto& item : *directories) {
        if (!item.is_string()) fail();
        const auto& dir = item.get_ref<const std::string&>();
        const fs::path path(dir);
        if (!profile_relative_path_valid(dir) || path.has_root_name() || path.lexically_normal().generic_string() != dir ||
            (!result.directories.empty() && dir <= result.directories.back()) ||
            std::any_of(dir.begin(), dir.end(), [](unsigned char c) { return c < 0x20 || c == 0x7f; })) fail();
        result.directories.push_back(dir);
    }
    return result;
}

} // namespace shin::context
