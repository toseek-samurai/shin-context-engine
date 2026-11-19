#include "context_internal.h"
#include "source_read.h"
#include <re2/re2.h>

namespace shin::context {

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

static int alnum_count(const std::string& value) {
    int count = 0;
    for (unsigned char c : value) {
        if (std::isalnum(c) != 0) {
            ++count;
        }
    }
    return count;
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

namespace {
using re2::RE2;
using Piece = re2::StringPiece;

RE2::Options scanner_options() {
    RE2::Options options;
    options.set_encoding(RE2::Options::EncodingLatin1);
    options.set_log_errors(false);
    options.set_max_mem(256 * 1024);
    return options;
}

struct SecretPatterns {
    RE2 private_key{R"(-----BEGIN ((?:[A-Z0-9 -]+ )?PRIVATE KEY)-----)", scanner_options()};
    RE2 aws{R"((?:^|[^A-Za-z0-9_])((?:AKIA|ASIA)[A-Za-z0-9]{16})(?:$|[^A-Za-z0-9_]))", scanner_options()};
    RE2 openai{R"((?:^|[^A-Za-z0-9_])(sk-[A-Za-z0-9_./+=-]{21,}))", scanner_options()};
    RE2 jwt{R"((?:^|[^A-Za-z0-9_])(eyJ[A-Za-z0-9_-]+\.[A-Za-z0-9_-]+\.[A-Za-z0-9_./+=-]+))", scanner_options()};
    RE2 bearer{R"((?i:\bbearer)[ \t]+([A-Za-z0-9_./+=-]{20,}))", scanner_options()};
    RE2 assignment{R"((?:^|[^A-Za-z0-9_$])["']?((?i:(?:[A-Za-z_$][A-Za-z0-9_$.-]*)?(?:secret|password|token|api_?key|access_key|private_key)(?:value)?))["']?[ \t]*[:=][ \t]*)", scanner_options()};
    RE2 dotenv_key{R"([A-Za-z_][A-Za-z0-9_]*)", scanner_options()};
    RE2 dotenv_next_assignment{R"(^\r?\n[ \t]*[A-Za-z_][A-Za-z0-9_]*[ \t]*=)", scanner_options()};
    RE2 literals{R"re("((?:\\(?s:.)|[^"\\])*)(?:"|$)|'((?:\\(?s:.)|[^'\\])*)(?:'|$)|`((?:\\(?s:.)|[^`\\])*)(?:`|$))re", scanner_options()};
    RE2 bare{R"(^[ \t\r\n]*([A-Za-z0-9_./+=-]{16,})[ \t]*(?:$|[,;}\r\n]))", scanner_options()};
    RE2 manifest_version{R"((?m)^[ \t]*apiVersion:[ \t]+[A-Za-z0-9./_-]+[ \t]*\r?$)", scanner_options()};
    RE2 manifest_kind{R"((?m)^[ \t]*kind:[ \t]+[A-Za-z][A-Za-z0-9]*[ \t]*\r?$)", scanner_options()};
    RE2 manifest_name{R"((?m)^[ \t]*metadata:[ \t]*\{[ \t]*name:[ \t]*([a-z0-9](?:[-a-z0-9.]*[a-z0-9])?)[ \t]*[,}])", scanner_options()};

    SecretPatterns() {
        for (const auto* rule : {&private_key, &aws, &openai, &jwt, &bearer, &assignment,
                                &dotenv_key, &dotenv_next_assignment, &literals, &bare,
                                &manifest_version, &manifest_kind, &manifest_name})
            if (!rule->ok()) throw std::logic_error("Invalid built-in secret scanner rule");
    }
};

bool dotenv_source(std::string_view path) {
    return path.ends_with(".env") || path.ends_with(".env.example") ||
        path.ends_with(".env.sample") || path.ends_with(".env.template") || path.ends_with(".env.dist");
}

bool empty_dotenv_assignment(Piece input, Piece key, Piece remainder, const SecretPatterns& rules) {
    const auto key_start = static_cast<std::size_t>(key.data() - input.data());
    const auto separator = key_start + key.size();
    return (key_start == 0 || input[key_start - 1] == '\n') &&
        separator < input.size() && input[separator] == '=' &&
        RE2::FullMatch(key, rules.dotenv_key) && RE2::PartialMatch(remainder, rules.dotenv_next_assignment);
}

// Keep value inspection local. Newlines in literals/continued expressions are
// retained, but a declaration cannot borrow identifiers from later statements.
Piece assignment_value(Piece input, bool& unfinished) {
    std::size_t start = 0;
    while (start < input.size() && is_ascii_space(input[start])) ++start;
    char quote = 0;
    bool literal_seen = false;
    bool escaped = false;
    unsigned parentheses = 0;
    char previous = 0;
    for (std::size_t pos = start; pos < input.size(); ++pos) {
        const char c = input[pos];
        if (quote) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == quote) quote = 0;
            continue;
        }
        if (c == '"' || c == '\'' || c == '`') {
            // A URL/query assignment may occur inside a surrounding literal.
            // Its closing quote ends this value; it must not open a string that
            // consumes the following code. Literal operands still scan normally.
            if (pos != start && std::string_view("(,:=+?@$>)").find(previous) == std::string_view::npos &&
                !(literal_seen && (previous == '"' || previous == '\'' || previous == '`'))) {
                unfinished = parentheses != 0;
                return input.substr(start, pos - start);
            }
            quote = c;
            literal_seen = true;
        } else if (c == '(' || c == '[') ++parentheses;
        else if ((c == ')' || c == ']') && parentheses) --parentheses;
        else if (c == ';' || (c == ',' && !parentheses) ||
                 ((c == '\r' || c == '\n') && !parentheses && previous != '+' && previous != '|')) {
            unfinished = quote || parentheses;
            return input.substr(start, pos - start);
        }
        if (!is_ascii_space(c)) previous = c;
    }
    unfinished = quote || parentheses;
    return input.substr(start);
}

bool secret_value(Piece input, const SecretPatterns& rules, std::set<std::string>* copies = nullptr) {
    std::string value(input);
    // A resource identifier is not a credential value. Only omit the name
    // scalar in a recognized manifest's flow metadata; all other fields and
    // every provider signature remain scanned, including data/stringData.
    if (value.find("apiVersion:") != std::string::npos && value.find("metadata:") != std::string::npos &&
        RE2::PartialMatch(input, rules.manifest_version) && RE2::PartialMatch(input, rules.manifest_kind)) {
        Piece remaining = input, name;
        while (RE2::FindAndConsume(&remaining, rules.manifest_name, &name)) {
            const auto offset = static_cast<std::size_t>(name.data() - input.data());
            std::fill_n(value.begin() + offset, name.size(), ' ');
        }
    }
    bool found = false;
    for (const auto& token : secret_candidate_tokens(value)) {
        if (!looks_like_assigned_secret_value(token)) continue;
        if (!copies) return true;
        copies->insert(token);
        found = true;
    }
    return found;
}

std::optional<Piece> query_fragment_tail(Piece input, Piece key, Piece remaining) {
    const auto offset = static_cast<std::size_t>(key.data() - input.data());
    if (!offset || (input[offset - 1] != '?' && input[offset - 1] != '&') || remaining.empty() ||
        (remaining.front() != '"' && remaining.front() != '\'' && remaining.front() != '`')) return std::nullopt;
    // The quote closes an empty URL parameter. Do not let it open a literal
    // spanning subsequent statements. Retain concatenated value operands.
    remaining.remove_prefix(1);
    while (!remaining.empty() && (remaining.front() == ' ' || remaining.front() == '\t' ||
           remaining.front() == ')' || remaining.front() == ']')) remaining.remove_prefix(1);
    if (!remaining.empty() && (remaining.front() == '\n' || remaining.front() == '\r')) {
        auto continued = remaining;
        while (!continued.empty() && is_ascii_space(continued.front())) continued.remove_prefix(1);
        if (!continued.empty() && continued.front() == '+') return continued;
    }
    if (remaining.empty() || remaining.front() == '\n' || remaining.front() == '\r' ||
        remaining.front() == ';' || remaining.front() == ',' || remaining.front() == '}') return Piece{};
    if (remaining.front() == '+') return remaining;
    return std::nullopt;
}

template <typename Predicate, typename Observe>
bool matched_token(Piece input, const RE2& rule, Predicate accept, Observe observe) {
    Piece token;
    for (;;) {
        const Piece previous = input;
        if (!RE2::FindAndConsume(&input, rule, &token)) return false;
        // Some signatures consume a trailing delimiter. Reuse it as the
        // possible leading delimiter of a second adjacent credential.
        input = previous.substr(static_cast<std::size_t>(token.data() + token.size() - previous.data()));
        if (!is_placeholder_secret_text(std::string(token)) && accept(token) && observe(token)) return true;
    }
}

template <typename Observe>
bool private_key_material(Piece input, const SecretPatterns& rules, Observe observe) {
    Piece kind;
    while (RE2::FindAndConsume(&input, rules.private_key, &kind)) {
        auto body = input;
        std::size_t bytes = 0;
        while (!body.empty()) {
            const auto c = static_cast<unsigned char>(body.front());
            if (is_ascii_space(c)) body.remove_prefix(1);
            else if (c == '\\') {
                // Serialized source/JSON may escape a line break more than once.
                std::size_t escape = 1;
                while (escape < body.size() && body[escape] == '\\') ++escape;
                if (escape == body.size() || (body[escape] != 'n' && body[escape] != 'r')) break;
                body.remove_prefix(escape + 1);
            }
            else if (c == '"' || c == '\'') {
                // Keep literal PEM bodies split by source concatenation. A call
                // to an encoder/generator is not a literal continuation.
                auto next = body.substr(1);
                while (!next.empty() && is_ascii_space(next.front())) next.remove_prefix(1);
                if (!next.empty() && next.front() == '+') {
                    next.remove_prefix(1);
                    while (!next.empty() && is_ascii_space(next.front())) next.remove_prefix(1);
                }
                if (next.empty() || next.front() != c) break;
                next.remove_prefix(1);
                body = next;
            } else if (std::isalnum(c) || c == '+' || c == '/' || c == '=') {
                ++bytes;
                body.remove_prefix(1);
            } else break;
        }
        // A marker used to construct a key at runtime contains no key material.
        // Keep complete PEM bodies and substantial truncated/escaped bodies.
        const auto terminator = "-----END " + std::string(kind) + "-----";
        if (bytes >= 64 || (bytes > 0 && body.starts_with(terminator))) {
            const auto* first = kind.data() - std::string_view("-----BEGIN ").size();
            const auto* last = body.data() + (body.starts_with(terminator) ? terminator.size() : 0);
            if (observe(Piece(first, static_cast<std::size_t>(last - first)))) return true;
        }
    }
    return false;
}

bool ternary_branch(Piece input, Piece key, Piece remaining) {
    auto separator = static_cast<std::size_t>(remaining.data() - input.data());
    while (separator && is_ascii_space(input[separator - 1])) --separator;
    if (!separator || input[separator - 1] != ':') return false;
    auto before = static_cast<std::size_t>(key.data() - input.data());
    while (before && is_ascii_space(input[before - 1])) --before;
    // `condition ? token : expression` reads token; it does not assign a value.
    // Object fields (`? {token: ...}`) and URL assignments remain inspected.
    if (!before || input[before - 1] != '?') return false;
    auto condition = before - 1;
    while (condition && (input[condition - 1] == ' ' || input[condition - 1] == '\t')) --condition;
    return condition && (std::isalnum(static_cast<unsigned char>(input[condition - 1])) ||
        std::string_view("_$)]\"'").find(input[condition - 1]) != std::string_view::npos);
}
enum class SecretRule { private_key, aws, openai, jwt, bearer, assignment };
constexpr std::array kSecretRuleNames = {std::string_view("private-key-block"), std::string_view("aws-access-key-id"),
    std::string_view("openai-api-key"), std::string_view("jwt-token"), std::string_view("bearer-token"),
    std::string_view("credential-assignment")};

constexpr std::size_t rule_index(SecretRule rule) {
    return static_cast<std::size_t>(rule);
}

using CopyTokens = std::array<std::set<std::string>, kSecretRuleNames.size()>;

struct SecretMatch {
    std::size_t begin, end;
    SecretRule rule;
    bool copied;
};

template <typename Record>
bool scan_secret_ranges(std::string_view text, std::string_view source_path,
                        CopyTokens* copies, Record record) {
    static const SecretPatterns rules;
    const Piece input(text.data(), text.size());
    const auto any = [](Piece) { return true; };
    const auto token_rule = [&](const RE2& rule, auto accept, SecretRule reason) {
        return matched_token(input, rule, accept, [&](Piece token) {
            if (copies && token.size() >= 16) (*copies)[rule_index(reason)].emplace(token.data(), token.size());
            return record(reason, token);
        });
    };
    if (text.find("-----BEGIN ") != std::string::npos && private_key_material(input, rules,
        [&](Piece block) {
            if (copies)
                for (auto& token : secret_candidate_tokens(std::string(block)))
                    if (token.size() >= 16) (*copies)[rule_index(SecretRule::private_key)].insert(std::move(token));
            return record(SecretRule::private_key, block);
        })) return true;
    if ((text.find("AKIA") != std::string::npos || text.find("ASIA") != std::string::npos) &&
        token_rule(rules.aws, any, SecretRule::aws)) return true;
    if (text.find("sk-") != std::string::npos && token_rule(rules.openai, any, SecretRule::openai)) return true;
    if (text.find("eyJ") != std::string::npos &&
        token_rule(rules.jwt, [](Piece value) { return value.size() >= 40; }, SecretRule::jwt)) return true;
    if (contains_ascii_case_insensitive(text, "bearer") &&
        token_rule(rules.bearer, [](Piece value) { return alnum_count(std::string(value)) >= 16; }, SecretRule::bearer)) return true;

    Piece remaining = input, key;
    const bool dotenv = dotenv_source(source_path);
    auto* assignment_copies = copies ? &(*copies)[rule_index(SecretRule::assignment)] : nullptr;
    while (RE2::FindAndConsume(&remaining, rules.assignment, &key)) {
        if (key.size() > 128 || !credential_key_side_is_sensitive(std::string(key)) || remaining.empty() ||
            remaining.front() == '=' || remaining.front() == '>') continue;
        if (ternary_branch(input, key, remaining)) continue;
        // In dotenv, a new assignment starts a separate field. It will still
        // be scanned under its own key, along with the whole-file signatures.
        if (dotenv && empty_dotenv_assignment(input, key, remaining, rules)) continue;
        auto value = remaining;
        if (const auto tail = query_fragment_tail(input, key, remaining)) {
            if (tail->empty()) continue;
            value = *tail;
        }
        bool unfinished = false;
        const auto local = assignment_value(value, unfinished);
        const auto candidate = assignment_candidate_text(std::string(local));
        bool found = unfinished && secret_value(candidate, rules, assignment_copies);
        Piece literals(candidate.data(), candidate.size()), quoted, single, templated, bare;
        while ((!found || copies) && RE2::FindAndConsume(&literals, rules.literals, &quoted, &single, &templated)) {
            if (secret_value(quoted, rules, assignment_copies) || secret_value(single, rules, assignment_copies) ||
                secret_value(templated, rules, assignment_copies)) found = true;
        }
        if ((!found || copies) && RE2::PartialMatch(candidate, rules.bare, &bare) &&
            secret_value(bare, rules, assignment_copies)) found = true;
        if (found && record(SecretRule::assignment,
            Piece(key.data(), static_cast<std::size_t>(local.data() + local.size() - key.data())))) return true;
    }
    return false;
}

void find_copied_values(std::string_view text, const std::set<std::string>& values,
                        SecretRule reason, std::vector<SecretMatch>& matches) {
    // Within a rule, a shorter matching token protects its longer variants on
    // the same line. The predecessor lookup then needs one comparison.
    std::vector<std::string_view> tokens;
    std::array<bool, 256> starts{};
    for (const auto& token : values) {
        if (!tokens.empty() && std::string_view(token).starts_with(tokens.back())) continue;
        tokens.emplace_back(token);
        starts[static_cast<unsigned char>(token.front())] = true;
    }
    for (std::size_t offset = 0; offset < text.size(); ++offset) {
        if (!starts[static_cast<unsigned char>(text[offset])]) continue;
        const auto suffix = text.substr(offset);
        const auto after = std::upper_bound(tokens.begin(), tokens.end(), suffix);
        if (after == tokens.begin()) continue;
        const auto token = *std::prev(after);
        if (!suffix.starts_with(token)) continue;
        matches.push_back({offset, offset + token.size(), reason, true});
        offset = text.find('\n', offset);
        if (offset == std::string_view::npos) break;
    }
}

SecretScanHit scan_secret_text(std::string_view text, std::string_view path,
                              std::vector<SecretMatch>* matches, bool include_physical_lines) {
    SecretScanHit hit;
    CopyTokens copies;
    const auto record = [&](SecretRule reason, Piece sensitive) {
        if (!hit.found) hit = {true, std::string(kSecretRuleNames[rule_index(reason)])};
        if (!matches) return true;
        const auto start = static_cast<std::size_t>(sensitive.data() - text.data());
        matches->push_back({start, start + sensitive.size(), reason, false});
        return false;
    };
    const auto scan = [&](std::string_view part) {
        return scan_secret_ranges(part, path, matches ? &copies : nullptr, record);
    };
    if (scan(text)) return hit;
    // Audit and publication must agree even when an isolated line looks
    // sensitive only without its enclosing expression.
    if (include_physical_lines) {
        auto lines = text;
        auto boundary = lines.find('\n');
        if (boundary != std::string_view::npos) {
            while (!lines.empty()) {
                auto line = lines.substr(0, boundary);
                if (line.ends_with('\r')) line.remove_suffix(1);
                if (!line.empty() && scan(line)) return hit;
                if (boundary == std::string_view::npos) break;
                lines.remove_prefix(boundary + 1);
                boundary = lines.find('\n');
            }
        }
    }
    if (matches)
        for (std::size_t i = 0; i < copies.size(); ++i)
            if (!copies[i].empty()) find_copied_values(text, copies[i], static_cast<SecretRule>(i), *matches);
    return hit;
}

Json rule_names(unsigned rules) {
    Json names = Json::array();
    for (std::size_t i = 0; i < kSecretRuleNames.size(); ++i)
        if (rules & (1u << i)) names.push_back(kSecretRuleNames[i]);
    return names;
}

std::vector<SourceRedaction::Range> redaction_ranges(const std::string& content,
                                                    const std::vector<SecretMatch>& matches) {
    if (matches.empty()) return {};
    constexpr std::size_t rule_count = kSecretRuleNames.size();
    const auto offsets = source_line_offsets(content);
    struct Boundary {
        std::size_t offset, rule;
        int delta;
    };
    std::vector<Boundary> boundaries;
    boundaries.reserve(matches.size() * 2);
    for (const auto& match : matches) {
        auto end = match.end;
        while (end > match.begin && is_ascii_space(content[end - 1])) --end;
        if (end == match.begin) continue;
        const auto first = std::prev(std::upper_bound(offsets.begin(), offsets.end(), match.begin));
        const auto after = std::upper_bound(offsets.begin(), offsets.end(), end - 1);
        const auto rule = rule_index(match.rule) + (match.copied ? rule_count : 0);
        boundaries.push_back({*first, rule, 1});
        boundaries.push_back({after == offsets.end() ? content.size() : *after, rule, -1});
    }
    std::sort(boundaries.begin(), boundaries.end(), [](const auto& a, const auto& b) {
        return a.offset < b.offset;
    });
    // Split whenever the active rule sets change. Overlapping matches keep
    // their attribution on each affected line.
    std::array<int, rule_count * 2> active{};
    std::vector<SourceRedaction::Range> ranges;
    std::size_t begin = 0;
    for (std::size_t i = 0; i < boundaries.size();) {
        const auto end = boundaries[i].offset;
        unsigned rules = 0, copied_rules = 0;
        for (std::size_t index = 0; index < rule_count; ++index) {
            if (active[index] > 0) rules |= 1u << index;
            else if (active[rule_count + index] > 0) copied_rules |= 1u << index;
        }
        if (begin < end && (rules || copied_rules)) {
            if (!ranges.empty() && ranges.back().end == begin &&
                ranges.back().rules == rules && ranges.back().copied_rules == copied_rules)
                ranges.back().end = end;
            else ranges.push_back({begin, end, rules, copied_rules});
        }
        do {
            const auto& boundary = boundaries[i++];
            active[boundary.rule] += boundary.delta;
        } while (i < boundaries.size() && boundaries[i].offset == end);
        begin = end;
    }
    return ranges;
}
}  // namespace

SecretScanHit detect_secret_like_text(const std::string& text, bool allow_source_suppression, std::string_view source_path) {
    if (allow_source_suppression && contains_ascii_case_insensitive(text, "gitleaks:allow")) return {};
    return scan_secret_text(text, source_path, nullptr, false);
}

SecretScanHit source_range_secret(const std::string& text, std::string_view path) {
    return scan_secret_text(text, path, nullptr, true);
}

std::string_view source_redaction_rule(std::string_view reason) {
    for (const auto rule : kSecretRuleNames) if (rule == reason) return rule;
    return "secret-like-source-content";
}

bool SourceRedaction::intersects(std::size_t begin, std::size_t end) const {
    const auto first = std::lower_bound(ranges.begin(), ranges.end(), begin,
        [](const auto& range, std::size_t offset) { return range.end <= offset; });
    return begin < end && first != ranges.end() && first->begin < end;
}

void SourceRedaction::annotate_body(Json& body, const std::vector<std::size_t>& offsets,
                                    std::size_t begin, std::size_t end) const {
    body["state"] = "complete";
    if (begin >= end) return;
    const auto first = std::lower_bound(ranges.begin(), ranges.end(), begin,
        [](const auto& range, std::size_t offset) { return range.end <= offset; });
    const auto line_of = [&](std::size_t offset) {
        return static_cast<std::size_t>(std::upper_bound(offsets.begin(), offsets.end(), offset) - offsets.begin());
    };
    Json hidden = Json::array();
    for (auto range = first; range != ranges.end() && range->begin < end; ++range) {
        hidden.push_back({{"start_line", line_of(std::max(begin, range->begin))},
            {"end_line", line_of(std::min(end, range->end) - 1)},
            {"rules", rule_names(range->rules)}, {"copied_rules", rule_names(range->copied_rules)}});
    }
    if (hidden.empty()) return;
    const auto& first_range = hidden.front();
    const auto& rules = first_range.at("rules");
    body["state"] = "redacted";
    body["reason"] = rules.empty() ? first_range.at("copied_rules").front() : rules.front();
    body["redacted_line_ranges"] = std::move(hidden);
}

SourceRedaction redact_source(std::string& content, std::string_view path) {
    SourceRedaction result;
    std::vector<SecretMatch> matches;
    result.reason = scan_secret_text(content, path, &matches, true).kind;
    result.ranges = redaction_ranges(content, matches);
    for (const auto& range : result.ranges)
        for (auto pos = range.begin; pos < range.end; ++pos)
            if (content[pos] != '\n' && content[pos] != '\r') content[pos] = ' ';
    return result;
}

std::string redact_sensitive_text(const std::string& text) {
    const SecretScanHit hit = detect_secret_like_text(text);
    if (!hit.found) {
        return text;
    }
    return "[REDACTED:" + hit.kind + "]";
}

}  // namespace shin::context
