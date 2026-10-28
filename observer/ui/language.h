#pragma once
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <iterator>
#include <memory>
#include <string_view>
#include <string>

namespace shin::ui {
enum class Language { en, ru };
struct Translation { const char* en; const char* ru; };
inline constexpr Translation messages[] = {
#include "messages.inc"
};
// UI preference only. Never changes protocol keys, identifiers or process locale.
inline std::atomic<Language> current_language{Language::en};
inline Language language_for(std::string_view locale) {
    return locale.size() >= 2 && (locale[0] == 'r' || locale[0] == 'R') &&
        (locale[1] == 'u' || locale[1] == 'U') &&
        (locale.size() == 2 || locale[2] == '-' || locale[2] == '_' || locale[2] == '.' || locale[2] == '@')
        ? Language::ru : Language::en;
}
inline bool supported_language(std::string_view locale) {
    if (language_for(locale) == Language::ru) return true;
    return locale.size() >= 2 && (locale[0] == 'e' || locale[0] == 'E') && (locale[1] == 'n' || locale[1] == 'N') &&
        (locale.size() == 2 || locale[2] == '-' || locale[2] == '_' || locale[2] == '.' || locale[2] == '@');
}
inline Language resolve_language(std::string_view preference, std::string_view system) {
    return language_for(preference == "auto" || preference.empty() ? system : preference);
}
inline void set_language(Language language) { current_language.store(language, std::memory_order_relaxed); }
inline Language language() { return current_language.load(std::memory_order_relaxed); }
// Preserve compiler format checking across translated printf-style messages.
#if defined(__clang__) || defined(__GNUC__)
__attribute__((format_arg(1)))
#endif
inline const char* tr(const char* english) {
    if (language() == Language::en) return english;
    const std::string_view key(english);
    const auto found = std::lower_bound(std::begin(messages), std::end(messages), key,
        [](const Translation& entry, std::string_view value) { return std::string_view(entry.en) < value; });
    return found != std::end(messages) && found->en == key ? found->ru : english;
}
// Display numbers only; never apply this to identifiers or persisted values.
inline std::string decimal(std::string value) {
    if (language() == Language::ru) std::replace(value.begin(), value.end(), '.', ',');
    return value;
}
// Called at terminal startup, before worker threads. Empty variables are ignored.
inline Language terminal_language(std::string_view preference) {
    for (const auto* name : {"LC_ALL", "LC_MESSAGES", "LANG"}) {
#ifdef _MSC_VER
        char* buffer = nullptr;
        if (_dupenv_s(&buffer, nullptr, name) != 0) continue;
        const std::unique_ptr<char, decltype(&std::free)> owned(buffer, &std::free);
        const auto* value = owned.get();
#else
        const auto* value = std::getenv(name);
#endif
        if (value && *value) return resolve_language(preference, value);
    }
    return resolve_language(preference, "en");
}
} // namespace shin::ui
