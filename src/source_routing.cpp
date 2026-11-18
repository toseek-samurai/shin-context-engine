#include "source_routing.h"
#include "context_internal.h"
#include "source_syntax.h"

namespace shin::context {
bool exceeds_eager_source_work(const SourceFactPathResult& paths) {
    // Opening every file a second time to build a cold reader costs work even
    // when the files are small. Use the vocabulary from the verified scan for
    // large fan-out; this switches representation, never file eligibility.
    constexpr std::size_t eager_file_opens = 8192;
    return paths.text_bytes > kSourceIndexBytes || paths.paths.size() > eager_file_opens;
}

bool needs_source_file_routing(const SourceFactPathResult& paths) {
    // Adjacent final chunks cannot fit in one 1 KiB window: the emitter merges
    // them whenever they can. Pairing neighbors gives a safe corpus-wide upper
    // bound even for tiny syntax nodes, with one unpaired row per file.
    return exceeds_eager_source_work(paths) || paths.paths.size() + paths.text_bytes / 512 >= kSourceIndexRows;
}

std::string source_search_projection(std::string_view text) {
    std::string additions;
    std::size_t at = 0;
    const auto identifier_byte = [](unsigned char byte) { return is_ascii_alnum(byte) || byte == '_'; };
    while (at < text.size()) {
        if (!identifier_byte(static_cast<unsigned char>(text[at]))) { ++at; continue; }
        const auto start = at;
        while (at < text.size() && identifier_byte(static_cast<unsigned char>(text[at]))) ++at;
        const auto token = text.substr(start, at - start);
        bool compound = false;
        for (std::size_t i = 1; i < token.size(); ++i) compound = compound || identifier_word_boundary(token, i);
        if (!compound) continue;
        if (!additions.empty()) additions += ' ';
        for (std::size_t i = 0; i < token.size(); ++i) {
            if (identifier_word_boundary(token, i)) additions += ' ';
            additions += token[i];
        }
    }
    return std::string(text) + (additions.empty() ? "" : "\n" + additions);
}

std::string source_routing_terms(const std::string& path, const std::string& content, std::size_t& chunks) {
    if (content.empty()) return {};
    const auto plan = chunk_source_syntax(content, extension_of(path));
    if (!plan.coverage_complete)
        throw_index_error("source-discovery-incomplete-source", "Source routing requires complete bounded source windows.");
    chunks = plan.chunks.size();
    std::set<std::string> terms;
    const auto collect = [&](std::string_view text) {
        // ASCII punctuation/space separates unicode61 terms. Keep non-ASCII
        // bytes intact for the same native tokenizer and Porter stemmer.
        const auto token_byte = [](unsigned char c) { return c >= 0x80 || is_ascii_alnum(c); };
        for (std::size_t i = 0; i < text.size();) {
            if (!token_byte(static_cast<unsigned char>(text[i]))) { ++i; continue; }
            const auto start = i++;
            while (i < text.size() && token_byte(static_cast<unsigned char>(text[i]))) ++i;
            terms.emplace(text.substr(start, i - start));
        }
    };
    collect(source_search_projection(path));
    for (const auto& chunk : plan.chunks)
        collect(source_search_projection(std::string_view(content).substr(chunk.begin, chunk.end - chunk.begin)));
    std::string vocabulary;
    for (const auto& term : terms) {
        if (!vocabulary.empty()) vocabulary.push_back(' ');
        vocabulary += term;
    }
    return vocabulary;
}
} // namespace shin::context
