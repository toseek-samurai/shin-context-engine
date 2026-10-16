#include "context_internal.h"
#include "generated_row_decoder.h"
#include "snapshot_verification.h"
#include "source_read.h"
#include "source_syntax.h"

#include <iostream>

namespace shin::context {
namespace {

constexpr std::size_t kTextBytes = 2048;
constexpr std::size_t kFileBytes = 1024 * 1024;
constexpr int kChunkLines = 24;
constexpr std::size_t kChunkBytes = 1024;
constexpr std::size_t kChunksPerRange = 16;
using Range = std::pair<int, int>;

void redact_model_input(const fs::path& root, const fs::path& input_path) {
    constexpr std::uintmax_t maximum_bytes = 300ULL * 1024 * 1024;
    if (!fs::is_regular_file(input_path) || fs::file_size(input_path) > maximum_bytes)
        throw std::runtime_error("Invalid model input file");
    std::ifstream input(input_path, std::ios::binary);
    if (!input) throw std::runtime_error("Missing model input");
    std::string line, previous_path, previous_hash, content;
    std::vector<std::size_t> offsets;
    std::size_t count = 0;
    std::uintmax_t input_bytes = 0, source_bytes = 0;
    while (std::getline(input, line)) {
        input_bytes += line.size() + 1;
        if (line.size() > kFileBytes || input_bytes > maximum_bytes || count == 130000)
            throw std::runtime_error("Model input budget exceeded");
        const Json row = Json::parse(line);
        if (!row.is_object() || row.size() != 6 || !row.at("index").is_number_integer() || row.at("index") != count ||
            !row.at("text").is_string() || !row.at("start_line").is_number_integer() || !row.at("end_line").is_number_integer())
            throw std::runtime_error("Invalid model input row");
        const auto path = row.at("path").get<std::string>();
        const auto hash = row.at("source_content_hash").get<std::string>();
        if (path != previous_path || hash != previous_hash) {
            content = read_bound_source(root, path, hash, 16ULL * 1024 * 1024);
            source_bytes += content.size();
            if (source_bytes > maximum_bytes) throw std::runtime_error("Model source budget exceeded");
            offsets = source_line_offsets(content);
            previous_path = path;
            previous_hash = hash;
        }
        // Inspect the full source span as well as published text, including
        // values hidden by summary clipping or source suppression comments.
        auto hit = source_range_secret(source_physical_range(content, offsets, row.at("start_line").get<int>(), row.at("end_line").get<int>()));
        std::string text = row.at("text").get<std::string>();
        if (!hit.found) hit = source_range_secret(text);
        if (hit.found) text = "[REDACTED:" + hit.kind + "]";
        std::cout << Json({{"index", count}, {"text", text}, {"redacted", hit.found}}).dump() << '\n';
        ++count;
    }
    if (!input.eof() || !std::cout) throw std::runtime_error("Incomplete model input projection");
    std::cout << Json({{"type", "complete"}, {"records", count}}).dump() << '\n';
}

Json project_range(const FactRow& row, const std::string& content,
                   const std::vector<std::size_t>& offsets, std::size_t text_limit = kTextBytes) {
    const std::string raw = source_physical_range(content, offsets, row.start_line, row.end_line);
    if (row.span_hash.empty() || fnv1a64(raw) != row.span_hash) {
        throw std::runtime_error("Published span does not match its source bytes");
    }
    // Inspect the complete range before compaction/clipping; source suppression
    // comments cannot authorize exporting a credential into model input.
    const SecretScanHit hit = source_range_secret(raw);
    std::string text = compact_whitespace(raw);
    if (hit.found) text = "[REDACTED:" + hit.kind + "]";
    const auto redacted_bytes = text.size();
    if (text.size() > text_limit) {
        std::size_t cut = text_limit;
        while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xc0) == 0x80) --cut;
        text.resize(cut);
    }
    return {{"path", row.path}, {"start_line", row.start_line}, {"end_line", row.end_line},
            {"record_id", row.id}, {"span_hash", row.span_hash}, {"range_origin", row.range_origin},
            {"text", text}, {"truncated", redacted_bytes > text_limit},
            {"redacted", hit.found || starts_with(text, "[REDACTED:")},
            {"source_range_bytes", raw.size()}, {"redacted_text_bytes", redacted_bytes}};
}

Json chunk_range(const FactRow& row, const std::string& content,
                 const std::vector<std::size_t>& offsets, const std::set<Range>& scopes,
                 const Json& projection) {
    const Range parent{row.start_line, row.end_line};
    Json result = {{"chunks", Json::array()}, {"owned_lines", 0}, {"delegated_lines", 0},
                   {"emitted_lines", 0}, {"omitted_lines", 0}, {"clipped_chunks", 0}};
    if (projection.at("redacted").get<bool>()) {
        result["status"] = "redacted-range";
        return result;
    }
    if (!scopes.contains(parent)) {
        result["status"] = "not-syntax-owned";
        return result;
    }
    if (row.end_line - row.start_line + 1 <= kChunkLines &&
        projection.at("redacted_text_bytes").get<std::size_t>() <= kChunkBytes) {
        result["status"] = "short-range";
        return result;
    }
    std::vector<Range> nested;
    for (const auto& scope : scopes) {
        if (scope == parent || scope.second < parent.first || scope.first > parent.second) continue;
        if (scope.first >= parent.first && scope.second <= parent.second) nested.push_back(scope);
        else if (!(scope.first <= parent.first && scope.second >= parent.second)) {
            result["status"] = "ambiguous-overlap";
            result["omitted_lines"] = parent.second - parent.first + 1;
            return result;
        }
    }
    // Syntax owners partition physical lines. Descendants retain their own
    // records; the enclosing class/function does not emit their text again.
    std::vector<Range> owned;
    int cursor = parent.first;
    for (const auto& child : nested) {
        if (child.first > cursor) owned.emplace_back(cursor, child.first - 1);
        cursor = std::max(cursor, child.second + 1);
    }
    if (cursor <= parent.second) owned.emplace_back(cursor, parent.second);
    int owned_lines = 0;
    for (const auto& segment : owned) owned_lines += segment.second - segment.first + 1;
    result["owned_lines"] = owned_lines;
    result["delegated_lines"] = parent.second - parent.first + 1 - owned_lines;
    int emitted = 0;
    int clipped = 0;
    for (const auto& segment : owned) {
        for (int start = segment.first; start <= segment.second;) {
            if (result["chunks"].size() == kChunksPerRange) break;
            int end = start;
            while (end < segment.second && end - start + 1 < kChunkLines &&
                   compact_whitespace(source_physical_range(content, offsets, start, end + 1)).size() <= kChunkBytes) ++end;
            FactRow child = row;
            child.start_line = start;
            child.end_line = end;
            child.span_hash = fnv1a64(source_physical_range(content, offsets, start, end));
            Json projected = project_range(child, content, offsets, kChunkBytes);
            projected.erase("record_id");
            projected["range_origin"] = "semantic-line-window";
            projected["parent_start_line"] = row.start_line;
            projected["parent_end_line"] = row.end_line;
            projected["parent_span_hash"] = row.span_hash;
            result["chunks"].push_back(std::move(projected));
            emitted += end - start + 1;
            clipped += result["chunks"].back().at("truncated").get<bool>() ? 1 : 0;
            start = end + 1;
        }
    }
    result["emitted_lines"] = emitted;
    result["omitted_lines"] = owned_lines - emitted;
    result["clipped_chunks"] = clipped;
    result["status"] = emitted == owned_lines && clipped == 0 ? "partitioned" : "bounded-incomplete";
    return result;
}

void export_structural_file(const std::string& path, const std::string& hash, const std::string& content) {
    const auto plan = chunk_source_syntax(content, fs::path(path).extension().string());
    const auto offsets = source_line_offsets(content);
    // Scan the entire file before partitioning: a secret can straddle pieces.
    const auto secret = source_range_secret(content);
    const auto covered = plan.chunks.empty() ? 0 : plan.chunks.back().end;
    std::cout << Json({{"type", "file"}, {"path", path}, {"source_content_hash", hash},
        {"supported", plan.supported}, {"syntax_complete", plan.syntax_complete},
        {"coverage_complete", plan.coverage_complete}, {"source_bytes", content.size()},
        {"covered_bytes", covered}, {"omitted_bytes", content.size() - covered},
        {"chunks", plan.chunks.size()}, {"visited_nodes", plan.visited_nodes}, {"redacted", secret.found}}).dump() << '\n';
    std::size_t index = 0;
    for (const auto& chunk : plan.chunks) {
        const auto raw = content.substr(chunk.begin, chunk.end - chunk.begin);
        const auto start = std::upper_bound(offsets.begin(), offsets.end(), chunk.begin) - offsets.begin();
        const auto end = std::upper_bound(offsets.begin(), offsets.end(), chunk.end - 1) - offsets.begin();
        std::cout << Json({{"type", "chunk"}, {"path", path}, {"source_content_hash", hash},
            {"index", index++}, {"byte_start", chunk.begin}, {"byte_end", chunk.end},
            {"start_line", start}, {"end_line", end}, {"span_hash", fnv1a64(raw)},
            {"range_origin", chunk.fallback ? "bounded-text-fallback" : "syntax-partition"},
            {"text", secret.found ? "[REDACTED:source-file]" : raw}, {"redacted", secret.found}}).dump() << '\n';
    }
}

void export_sources(const fs::path& root, const fs::path& out, bool chunks, bool structural) {
    const auto snapshot = load_verified_snapshot(out, 512ULL * 1024 * 1024, 256ULL * 1024 * 1024);
    const auto key = (kGeneratedDir / "facts.jsonl").generic_string();
    const std::string facts = read_file(out / key);
    if (facts.size() != snapshot.files.at(key).bytes ||
        fnv1a64(facts) != snapshot.files.at(key).content_hash) {
        throw std::runtime_error("Published facts changed during export");
    }
    std::map<std::string, std::vector<FactRow>> by_path;
    std::map<std::string, std::string> source_hashes;
    for (const Json& source : snapshot.manifest.at("source_fact_sources")) {
        if (json_string_value(source, "status") == "scanned") {
            source_hashes.emplace(json_string_value(source, "path"), json_string_value(source, "content_hash"));
        }
    }
    std::istringstream input(facts);
    std::string line;
    while (std::getline(input, line)) {
        auto row = decode_generated_fact_row(line);
        if (starts_with(row.source, "source-fact:") && !row.span_hash.empty()) {
            by_path[row.path].push_back(std::move(row));
        }
    }
    if (structural) {
        // File admission comes from the publication, not from whether an
        // extractor happened to produce a declaration for that file.
        for (const auto& [path, hash] : source_hashes) by_path.try_emplace(path);
    }
    Json binding = {{"type", "binding"}, {"snapshot_id", snapshot.snapshot_id},
        {"projection", structural ? "core-bound-structural-chunks-v1" : (chunks ? "core-bound-chunks-v1" : "core-bound-range-v1")},
        {"max_text_bytes", kTextBytes}, {"max_source_file_bytes", kFileBytes}};
    if (structural) {
        binding["max_text_bytes"] = 1024;
        binding["chunk_policy"] = {{"max_bytes", 1024}, {"max_chunks_per_file", 4096},
            {"max_source_bytes", 64ULL * 1024 * 1024}, {"ownership", "published-source-file"},
            {"coordinates", "utf8-bytes-end-exclusive-lines-inclusive"}, {"redaction", "whole-file-before-partition"}};
        binding["selected_source_files"] = source_hashes.size();
    }
    if (chunks) binding["chunk_policy"] = {{"max_lines", kChunkLines}, {"max_text_bytes", kChunkBytes},
        {"max_chunks_per_range", kChunksPerRange}, {"ownership", "syntax-range-minus-descendants"}};
    std::cout << binding.dump() << '\n';
    std::size_t count = 0;
    std::size_t source_bytes = 0;
    for (const auto& [path, rows] : by_path) {
        const auto found = source_hashes.find(path);
        if (found == source_hashes.end() || found->second.empty()) {
            throw std::runtime_error("Fact has no published source binding");
        }
        const std::string content = read_bound_source(root, path, found->second, kFileBytes);
        if (structural) {
            source_bytes += content.size();
            if (source_bytes > 64ULL * 1024 * 1024) throw std::runtime_error("Structural export source budget exceeded");
            export_structural_file(path, found->second, content);
            ++count;
            continue;
        }
        const auto offsets = source_line_offsets(content);
        std::map<Range, Json> ranges;
        std::set<Range> scopes;
        for (const FactRow& row : rows) {
            const auto range = std::pair{row.start_line, row.end_line};
            auto found_range = ranges.find(range);
            if (found_range == ranges.end()) {
                found_range = ranges.emplace(range, project_range(row, content, offsets)).first;
            }
            if (found_range->second.at("span_hash") != row.span_hash) {
                throw std::runtime_error("Identical ranges have inconsistent span hashes");
            }
            if (row.range_origin == "syntax") scopes.insert(range);
        }
        if (chunks) {
            std::vector<Range> sensitive;
            for (const auto& [range, projection] : ranges) {
                if (projection.at("redacted").get<bool>()) sensitive.push_back(range);
            }
            for (auto& [range, projection] : ranges) {
                if (std::any_of(sensitive.begin(), sensitive.end(), [&](const Range& other) {
                        return other.first <= range.second && other.second >= range.first;
                    })) {
                    projection["text"] = "[REDACTED:overlapping-source-range]";
                    projection["redacted"] = true;
                    projection["truncated"] = false;
                    projection["redacted_text_bytes"] = projection.at("text").get<std::string>().size();
                }
            }
        }
        std::map<Range, Json> partitions;
        for (const FactRow& row : rows) {
            const Range range{row.start_line, row.end_line};
            Json record = ranges.at(range);
            if (chunks) {
                auto partition = partitions.find(range);
                if (partition == partitions.end()) {
                    partition = partitions.emplace(range, chunk_range(row, content, offsets, scopes, record)).first;
                }
                record["chunking"] = partition->second;
                record["symbol"] = redact_sensitive_text(row.symbol_name);
            }
            record["record_id"] = row.id;
            record["range_origin"] = row.range_origin;
            record["source_content_hash"] = found->second;
            record["type"] = "range";
            std::cout << record.dump() << '\n';
            ++count;
        }
    }
    if (read_file(out / kGeneratedDir / "manifest.json") != snapshot.manifest_content) {
        throw std::runtime_error("Publication changed during export");
    }
    std::cout << Json({{"type", "complete"}, {"records", count}, {"source_files", by_path.size()},
                      {"snapshot_id", snapshot.snapshot_id}}).dump() << '\n';
    if (!std::cout) throw std::runtime_error("Export output failed");
}

} // namespace
} // namespace shin::context

int main(int argc, char** argv) {
    try {
        if (argc == 4 && std::string_view(argv[1]) == "--redact-model-input") {
            shin::context::redact_model_input(std::filesystem::canonical(argv[2]), std::filesystem::canonical(argv[3]));
            return std::cout ? 0 : 1;
        }
        const bool chunks = argc == 4 && std::string_view(argv[3]) == "--chunks";
        const bool structural = argc == 4 && std::string_view(argv[3]) == "--structural-chunks";
        if (argc != 3 && !chunks && !structural) {
            throw std::runtime_error("Usage: shin-semantic-source-export ROOT INDEX [--chunks|--structural-chunks]");
        }
        shin::context::export_sources(std::filesystem::canonical(argv[1]),
                                     std::filesystem::canonical(argv[2]), chunks, structural);
        return 0;
    } catch (const std::exception&) {
        // Source/parser diagnostics can include private text; never forward them.
        std::cerr << "Semantic source export rejected unsafe, stale or invalid input.\n";
        return 1;
    }
}
