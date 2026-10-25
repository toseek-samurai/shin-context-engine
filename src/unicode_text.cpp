#include "unicode_text.h"
#include "context_internal.h"
#include <utf8proc.h>

namespace shin::context {
namespace {
struct Codepoint { utf8proc_int32_t value; std::size_t bytes; };
Codepoint decode(std::string_view text, std::size_t at) {
    utf8proc_int32_t value = 0;
    const auto bytes = utf8proc_iterate(reinterpret_cast<const utf8proc_uint8_t*>(text.data() + at),
        static_cast<utf8proc_ssize_t>(text.size() - at), &value);
    return bytes > 0 ? Codepoint{value, static_cast<std::size_t>(bytes)} : Codepoint{-1, 1};
}
bool alnum(utf8proc_int32_t value) {
    const auto kind = utf8proc_category(value);
    return (kind >= UTF8PROC_CATEGORY_LU && kind <= UTF8PROC_CATEGORY_LO) ||
           (kind >= UTF8PROC_CATEGORY_ND && kind <= UTF8PROC_CATEGORY_NO);
}
bool mark(utf8proc_int32_t value) {
    const auto kind = utf8proc_category(value);
    return kind >= UTF8PROC_CATEGORY_MN && kind <= UTF8PROC_CATEGORY_ME;
}
bool space(utf8proc_int32_t value) {
    const auto kind = utf8proc_category(value);
    return (value >= 9 && value <= 13) || kind == UTF8PROC_CATEGORY_ZS ||
           kind == UTF8PROC_CATEGORY_ZL || kind == UTF8PROC_CATEGORY_ZP;
}
bool query_punctuation(utf8proc_int32_t value) {
    return value == '_' || value == '-' || value == '.' || value == '/' || value == '+' || value == '#';
}
std::string normalize_unicode(std::string_view text, int options) {
    const auto valid = unicode_query_spacing(text);
    utf8proc_uint8_t* raw = nullptr;
    const auto size = utf8proc_map(reinterpret_cast<const utf8proc_uint8_t*>(valid.data()),
        static_cast<utf8proc_ssize_t>(valid.size()), &raw, options);
    std::unique_ptr<utf8proc_uint8_t, decltype(&utf8proc_free)> output(raw, utf8proc_free);
    if (size < 0 || !output) throw_usage_error("unicode-normalization-failed", "Unicode lexical normalization failed.");
    return {reinterpret_cast<const char*>(output.get()), static_cast<std::size_t>(size)};
}
}

bool has_non_ascii(std::string_view text) noexcept {
    return std::any_of(text.begin(), text.end(), [](unsigned char c) { return c >= 128; });
}

std::string unicode_query_spacing(std::string_view text) {
    if (!has_non_ascii(text)) return std::string(text);
    std::string out;
    out.reserve(text.size());
    for (std::size_t at = 0; at < text.size();) {
        const auto cp = decode(text, at);
        if (cp.value < 0 || space(cp.value)) out += ' ';
        else out.append(text.substr(at, cp.bytes));
        at += cp.bytes;
    }
    return out;
}

std::string unicode_casefold(std::string_view text) {
    if (!has_non_ascii(text)) return to_lower(std::string(text));
    return normalize_unicode(text, UTF8PROC_STABLE | UTF8PROC_COMPOSE | UTF8PROC_CASEFOLD);
}

std::string unicode_trim_query_token(std::string_view text) {
    std::size_t begin = text.size(), end = 0;
    for (std::size_t at = 0; at < text.size();) {
        const auto cp = decode(text, at);
        if (alnum(cp.value) || query_punctuation(cp.value) || (begin != text.size() && mark(cp.value))) {
            begin = std::min(begin, at);
            end = at + cp.bytes;
        }
        at += cp.bytes;
    }
    return end ? std::string(text.substr(begin, end - begin)) : std::string();
}

std::string unicode_match_text(std::string_view text) {
    // Determine identifier boundaries on canonical characters, before folding
    // case. Decomposed accents must not hide the following lowercase letter.
    const auto canonical = normalize_unicode(text, UTF8PROC_STABLE | UTF8PROC_COMPOSE);
    text = canonical;
    std::string words;
    words.reserve(text.size());
    auto previous = UTF8PROC_CATEGORY_CN;
    bool boundary = true;
    for (std::size_t at = 0; at < text.size();) {
        const auto cp = decode(text, at);
        const auto kind = utf8proc_category(cp.value);
        const auto next = at + cp.bytes < text.size() ? utf8proc_category(decode(text, at + cp.bytes).value) : UTF8PROC_CATEGORY_CN;
        const bool camel = kind == UTF8PROC_CATEGORY_LU && !boundary &&
            (previous == UTF8PROC_CATEGORY_LL || previous == UTF8PROC_CATEGORY_ND ||
             (previous == UTF8PROC_CATEGORY_LU && next == UTF8PROC_CATEGORY_LL));
        if (camel) words += ' ';
        if (alnum(cp.value) || (mark(cp.value) && !boundary)) {
            words.append(text.substr(at, cp.bytes));
            boundary = false;
        } else if (!boundary) {
            words += ' ';
            boundary = true;
        }
        if (!mark(cp.value)) previous = kind;
        at += cp.bytes;
    }
    if (!words.empty() && words.back() == ' ') words.pop_back();
    return unicode_casefold(words);
}

bool unicode_index_term(std::string_view text) {
    std::size_t count = 0;
    bool non_ascii_word = false, in_word = false;
    for (std::size_t at = 0; at < text.size();) {
        const auto cp = decode(text, at);
        if (cp.value < 0) return false;
        if (alnum(cp.value)) { ++count; non_ascii_word |= cp.value >= 128; in_word = true; }
        else if (!(mark(cp.value) && in_word)) {
            if (!space(cp.value) && !query_punctuation(cp.value)) return false;
            in_word = false;
        }
        at += cp.bytes;
    }
    return count >= 2 || (count == 1 && non_ascii_word);
}

bool unicode_prefix_term(std::string_view text) {
    std::size_t letters = 0;
    for (std::size_t at = 0; at < text.size();) {
        const auto cp = decode(text, at);
        if (cp.value < 0 || (!alnum(cp.value) && !(mark(cp.value) && letters))) return false;
        if (alnum(cp.value)) ++letters;
        at += cp.bytes;
    }
    return letters >= 4;
}
}
