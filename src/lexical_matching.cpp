#include "context_internal.h"
#include <memory_resource>
#ifdef SHIN_EXPERIMENT_LEXICAL_REUSE
#include "lexical_reuse_experiment.h"
#endif

namespace shin::context {

std::vector<FieldMatchInput> section_match_fields(const SectionRow& row) {
    std::vector<FieldMatchInput> fields{
        {"document_id", row.document_id, 38, true},
        {"heading", row.heading, 34, true},
        {row.indexed_text.empty() ? "preview" : "indexed_text",
         row.indexed_text.empty() ? row.preview : row.indexed_text, 16, true},
        {"path", row.path, 14, true},
        {"status", row.status, 10, true},
        {"authority_level", row.authority_level, 8, true},
        {"row_type", row.row_type, 6, true},
    };
    for (const auto& topic : row.topics) fields.emplace_back("topics", topic, 4, false);
    return fields;
}

std::vector<FieldMatchInput> fact_match_fields(const FactRow& row) {
    std::vector<FieldMatchInput> fields{
        {"title", row.title, 36, true},
        {"summary", row.summary, 20, true},
        {"id", row.id, 22, true},
        {"record_type", row.record_type, 20, true},
        {"path", row.path, 12, true},
        {"status", row.status, 10, true},
        {"area", row.area, 10, true},
        {"authority_level", row.authority_level, 8, true},
    };
    fields.reserve(fields.size() + row.identifier_terms.size() + row.topics.size());
    // Each reference is an independent lexical field, never a concatenated phrase.
    // Full identifier confirmation is owned by the separate Brief identity probe.
    for (const auto& identifier : row.identifier_terms) {
        fields.emplace_back("identifier_references", identifier, 32, true);
    }
    for (const auto& topic : row.topics) fields.emplace_back("topics", topic, 3, false);
    return fields;
}

bool section_has_non_body_query_match(const SectionRow& row, const PreparedTermGroups& groups,
                                     bool include_document_identity) {
    for (auto& field : section_match_fields(row)) {
        if (!field.primary || field.name == "indexed_text" || field.name == "preview") continue;
        if (!include_document_identity && (field.name == "document_id" || field.name == "path")) continue;
        for (const auto& group : groups) {
            for (const auto& term : group) {
                if (field_contains_term(field, term)) return true;
            }
        }
    }
    return false;
}

bool index_term_has_signal(std::string_view term) {
    static constexpr auto stop_terms = std::to_array<std::string_view>({
        "a", "about", "an", "and", "are", "as", "at", "be", "been", "being", "by", "can",
        "could", "did", "do", "does", "for", "from", "how", "i", "in", "into", "is", "it",
        "me", "of", "on", "or", "please", "run", "show", "tell", "the", "to", "was", "were",
        "what", "when", "where", "which", "who", "why", "will", "with", "without", "would"
    });
    static_assert(std::is_sorted(stop_terms.begin(), stop_terms.end()));
    static_assert(std::all_of(stop_terms.begin(), stop_terms.end(), [](auto term) { return term.size() <= 7; }));
    if (term.empty() || term.size() > 128 ||
        (term.size() <= 7 && std::binary_search(stop_terms.begin(), stop_terms.end(), term))) {
        return false;
    }
    int alnum_count = 0;
    for (char raw : term) {
        const unsigned char c = static_cast<unsigned char>(raw);
        const bool alnum = is_ascii_alnum(c);
        if (alnum) {
            ++alnum_count;
        }
        if (!alnum && !is_ascii_space(c) && c != '_' && c != '-' && c != '.' &&
            c != '/' && c != '+' && c != '#') {
            return false;
        }
    }
    if (alnum_count >= 2) {
        return true;
    }
    return term == "c++" || term == "c#";
}

struct IndexTermHash {
    using is_transparent = void;
    std::size_t operator()(std::string_view value) const noexcept {
        return std::hash<std::string_view>{}(value);
    }
};

using IndexTermSet = std::pmr::unordered_set<std::pmr::string, IndexTermHash, std::equal_to<>>;

static void insert_index_term(IndexTermSet& terms, std::string_view value) {
    if (!terms.contains(value) && index_term_has_signal(value)) terms.emplace(value);
}

template <typename Work>
void for_each_normalized_word(std::string_view text, Work&& work) {
    std::size_t start = 0;
    while (start < text.size()) {
        while (start < text.size() && is_ascii_space(static_cast<unsigned char>(text[start]))) {
            ++start;
        }
        std::size_t end = start;
        while (end < text.size() && !is_ascii_space(static_cast<unsigned char>(text[end]))) {
            ++end;
        }
        if (end > start) {
            work(text.substr(start, end - start));
        }
        start = end;
    }
}

static void add_prepared_index_term(IndexTermSet& terms, const PreparedMatchTerm& prepared,
                                    bool compact_alias) {
    auto add = [&](std::string_view value) {
        insert_index_term(terms, value);
    };
    add(prepared.raw);
    add(prepared.normalized);
    if (compact_alias) add(prepared.compact);
    for_each_normalized_word(prepared.normalized, add);
}

static void add_index_term(IndexTermSet& terms, std::string_view raw) {
    const PreparedMatchTerm prepared = prepare_match_term(raw);
    add_prepared_index_term(terms, prepared, true);
    std::string previous;
    for_each_normalized_word(prepared.normalized, [&](std::string_view word) {
        if (!previous.empty()) {
            const auto compound = previous + std::string(word);
            insert_index_term(terms, compound);
        }
        previous = word;
    });
    for (std::size_t slash = raw.find('/'); slash != std::string_view::npos;
         slash = raw.find('/', slash + 1)) {
        for (const auto path_part : {raw.substr(0, slash), raw.substr(slash + 1)}) {
            const auto key = to_lower(std::string(path_part));
            insert_index_term(terms, key);
        }
    }
}

static void add_index_terms_from_text(IndexTermSet& terms, std::string_view text) {
    PreparedMatchTerm prepared_text;
    if (text.size() <= 96) prepared_text = prepare_match_term(text);
    else prepared_text.normalized = normalize_match_text(text);
    std::string previous;
    for_each_normalized_word(prepared_text.normalized, [&](std::string_view word) {
        if (!previous.empty()) {
            const auto pair = previous + " " + std::string(word);
            insert_index_term(terms, pair);
        }
        previous = word;
    });
    if (text.size() <= 96) {
        add_prepared_index_term(terms, prepared_text, false);
    } else {
        for_each_normalized_word(prepared_text.normalized, [&](std::string_view word) {
            insert_index_term(terms, word);
        });
    }

    const std::string_view view(text);
    std::size_t segment_start = std::string_view::npos;
    bool segment_needs_preparation = false;
    for (std::size_t i = 0; i <= view.size(); ++i) {
        const bool at_end = i == view.size();
        const unsigned char c = at_end ? 0 : static_cast<unsigned char>(view[i]);
        const bool segment_char = !at_end &&
                                  (is_ascii_alnum(c) || c == '_' || c == '-' || c == '+' || c == '#' || c == '.' || c == '/');
        const bool special_segment_char = c == '_' || c == '-' || c == '+' || c == '#' || c == '.' || c == '/';
        if (segment_char && segment_start == std::string_view::npos) {
            segment_start = i;
            segment_needs_preparation = special_segment_char;
        } else if (segment_char) {
            const unsigned char previous = static_cast<unsigned char>(view[i - 1]);
            const bool camel_boundary =
                is_ascii_upper(c) && (is_ascii_lower(previous) || is_ascii_digit(previous));
            segment_needs_preparation = segment_needs_preparation || special_segment_char || camel_boundary;
        } else if (!segment_char && segment_start != std::string_view::npos) {
            if (segment_needs_preparation) {
                add_index_term(terms, view.substr(segment_start, i - segment_start));
            }
            segment_start = std::string_view::npos;
            segment_needs_preparation = false;
        }
    }
}


std::vector<std::string> lexical_match_terms(std::string_view text, bool sorted) {
    alignas(std::max_align_t) std::array<std::byte, 8192> storage;
    std::pmr::monotonic_buffer_resource memory(storage.data(), storage.size());
    IndexTermSet terms{&memory};
    terms.reserve(std::clamp<std::size_t>(text.size() / 3, 32, 256));
    add_index_terms_from_text(terms, text);
    std::vector<std::string> result;
    result.reserve(terms.size());
    for (const auto& term : terms) result.emplace_back(term);
    if (sorted) std::sort(result.begin(), result.end());
    return result;
}

bool can_prefix_complete_lookup_term(std::string_view term) {
    constexpr std::size_t kMinPrefixBytes = 4;
    return term.size() >= kMinPrefixBytes &&
           std::all_of(term.begin(), term.end(), [](char raw) {
               return is_ascii_alnum(static_cast<unsigned char>(raw));
           });
}


// Every lexical key preserves a contiguous sequence of the field's ASCII
// letters/digits: normalization removes separators, but never intervening letters.
// KMP rejects scattered-letter false positives before vocabulary construction.
static bool could_contain_lexical_term(std::string_view text, std::string_view compact) {
    if (compact.empty()) return true;
    if (compact.size() > 128) {
        // Long phrase queries keep the original bounded-memory conservative probe.
        std::size_t matched = 0;
        for (const unsigned char raw : text) {
            if (static_cast<char>(ascii_lower(raw)) != compact[matched]) continue;
            if (++matched == compact.size()) return true;
        }
        return false;
    }
    std::array<std::size_t, 128> prefix;
    prefix[0] = 0;
    for (std::size_t i = 1, matched = 0; i < compact.size(); ++i) {
        while (matched && compact[i] != compact[matched]) matched = prefix[matched - 1];
        if (compact[i] == compact[matched]) ++matched;
        prefix[i] = matched;
    }
    std::size_t matched = 0;
    for (const unsigned char raw : text) {
        if (!is_ascii_alnum(raw)) continue;
        const auto lowered = static_cast<char>(ascii_lower(raw));
        while (matched && lowered != compact[matched]) matched = prefix[matched - 1];
        if (lowered == compact[matched]) ++matched;
        if (matched == compact.size()) return true;
    }
    return false;
}

LexicalMatchKind field_match_kind(FieldMatchInput& field, const PreparedMatchTerm& term) {
    if (field.text.empty() || term.raw.empty()) return LexicalMatchKind::none;
    // Once this field owns its complete vocabulary, binary lookup is cheaper
    // than rescanning its text for every later group/alias. The probes only
    // avoid vocabulary construction; they are not additional matching rules.
    if (!field.lexical_ready && !could_contain_lexical_term(field.text, term.compact)) return LexicalMatchKind::none;
    if (!field.lexical_ready && term.normalized == term.compact && index_term_has_signal(term.normalized)) {
        for (auto start = field.text.find(term.normalized); start != std::string_view::npos;
             start = field.text.find(term.normalized, start + 1)) {
            const auto end = start + term.normalized.size();
            const bool left = start == 0 || !is_ascii_alnum(static_cast<unsigned char>(field.text[start - 1])) ||
                identifier_word_boundary(field.text, start);
            const bool right = end == field.text.size() || !is_ascii_alnum(static_cast<unsigned char>(field.text[end])) ||
                identifier_word_boundary(field.text, end);
            if (left && right) return LexicalMatchKind::exact;
        }
    }
    if (field.name == "path" && field.text.size() == term.raw.size() &&
        contains_ascii_case_insensitive(field.text, term.raw)) return LexicalMatchKind::exact;
    if (!field.lexical_ready) {
#ifdef SHIN_EXPERIMENT_LEXICAL_REUSE
        field.lexical_terms = experiment::lexical_terms(field.text);
#else
        field.lexical_terms = lexical_match_terms(field.text);
#endif
        field.lexical_ready = true;
    }
    const auto& lexical_terms = field.lexical_terms;
    for (const std::string* key : {&term.raw, &term.normalized, &term.compact}) {
        const auto found = std::lower_bound(lexical_terms.begin(), lexical_terms.end(), *key);
        if (found != lexical_terms.end() && *found == *key) return LexicalMatchKind::exact;
    }
    if (!term.phrase_keys.empty()) {
        if (!field.normalized_ready) {
            field.normalized_text = normalize_match_text(field.text);
            field.normalized_ready = true;
        }
        for (auto offset = field.normalized_text.find(term.normalized); offset != std::string::npos;
             offset = field.normalized_text.find(term.normalized, offset + 1)) {
            const auto end = offset + term.normalized.size();
            if ((offset == 0 || field.normalized_text[offset - 1] == ' ') &&
                (end == field.normalized_text.size() || field.normalized_text[end] == ' ')) return LexicalMatchKind::exact;
        }
    }
    if (term.allow_prefix && can_prefix_complete_lookup_term(term.compact)) {
        auto found = std::lower_bound(lexical_terms.begin(), lexical_terms.end(), term.compact);
        while (found != lexical_terms.end() && starts_with(*found, term.compact)) {
            if (can_prefix_complete_lookup_term(*found)) return LexicalMatchKind::prefix;
            ++found;
        }
    }
    return LexicalMatchKind::none;
}

bool field_contains_term(FieldMatchInput& field, const PreparedMatchTerm& term) {
    return field_match_kind(field, term) != LexicalMatchKind::none;
}

} // namespace shin::context
