#include "context_internal.h"
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
    RE2 private_key{R"(-----BEGIN (?:[A-Z0-9 -]+ )?PRIVATE KEY-----)", scanner_options()};
    RE2 aws{R"((?:^|[^A-Za-z0-9_])((?:AKIA|ASIA)[A-Za-z0-9]{16})(?:$|[^A-Za-z0-9_]))", scanner_options()};
    RE2 openai{R"((?:^|[^A-Za-z0-9_])(sk-[A-Za-z0-9_./+=-]{21,}))", scanner_options()};
    RE2 jwt{R"((?:^|[^A-Za-z0-9_])(eyJ[A-Za-z0-9_-]+\.[A-Za-z0-9_-]+\.[A-Za-z0-9_./+=-]+))", scanner_options()};
    RE2 bearer{R"((?i:\bbearer)[ \t]+([A-Za-z0-9_./+=-]{20,}))", scanner_options()};
    RE2 assignment{R"((?:^|[^A-Za-z0-9_$])["']?((?i:(?:[A-Za-z_$][A-Za-z0-9_$.-]*)?(?:secret|password|token|api_?key|access_key|private_key)(?:value)?))["']?[ \t]*[:=][ \t]*)", scanner_options()};
    RE2 literals{R"re("((?:\\(?s:.)|[^"\\])*)(?:"|$)|'((?:\\(?s:.)|[^'\\])*)(?:'|$)|`((?:\\(?s:.)|[^`\\])*)(?:`|$))re", scanner_options()};
    RE2 bare{R"(^[ \t\r\n]*([A-Za-z0-9_./+=-]{16,})[ \t]*(?:$|[,;}\r\n]))", scanner_options()};

    SecretPatterns() {
        for (const auto* rule : {&private_key, &aws, &openai, &jwt, &bearer, &assignment, &literals, &bare})
            if (!rule->ok()) throw std::logic_error("Invalid built-in secret scanner rule");
    }
};

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

bool secret_value(Piece input) {
    for (const auto& token : secret_candidate_tokens(std::string(input)))
        if (looks_like_assigned_secret_value(token)) return true;
    return false;
}

template <typename Predicate>
bool matched_token(Piece input, const RE2& rule, Predicate accept) {
    Piece token;
    while (RE2::FindAndConsume(&input, rule, &token))
        if (!is_placeholder_secret_text(std::string(token)) && accept(token)) return true;
    return false;
}
}  // namespace

SecretScanHit detect_secret_like_text(const std::string& text, bool allow_source_suppression) {
    if (allow_source_suppression && contains_ascii_case_insensitive(text, "gitleaks:allow")) return {};
    static const SecretPatterns rules;
    const Piece input(text.data(), text.size());
    const auto any = [](Piece) { return true; };
    if (text.find("-----BEGIN ") != std::string::npos && RE2::PartialMatch(input, rules.private_key))
        return {true, "private-key-block"};
    if ((text.find("AKIA") != std::string::npos || text.find("ASIA") != std::string::npos) && matched_token(input, rules.aws, any))
        return {true, "aws-access-key-id"};
    if (text.find("sk-") != std::string::npos && matched_token(input, rules.openai, any))
        return {true, "openai-api-key"};
    if (text.find("eyJ") != std::string::npos && matched_token(input, rules.jwt, [](Piece value) { return value.size() >= 40; }))
        return {true, "jwt-token"};
    if (contains_ascii_case_insensitive(text, "bearer") &&
        matched_token(input, rules.bearer, [](Piece value) { return alnum_count(std::string(value)) >= 16; }))
        return {true, "bearer-token"};

    Piece remaining = input, key;
    while (RE2::FindAndConsume(&remaining, rules.assignment, &key)) {
        if (key.size() > 128 || !credential_key_side_is_sensitive(std::string(key)) || remaining.empty() ||
            remaining.front() == '=' || remaining.front() == '>') continue;
        bool unfinished = false;
        const auto local = assignment_value(remaining, unfinished);
        const auto candidate = assignment_candidate_text(std::string(local));
        if (unfinished && secret_value(candidate)) return {true, "credential-assignment"};
        Piece literals(candidate.data(), candidate.size()), quoted, single, templated, bare;
        while (RE2::FindAndConsume(&literals, rules.literals, &quoted, &single, &templated))
            if (secret_value(quoted) || secret_value(single) || secret_value(templated))
                return {true, "credential-assignment"};
        if (RE2::PartialMatch(candidate, rules.bare, &bare) && secret_value(bare))
            return {true, "credential-assignment"};
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

}  // namespace shin::context
