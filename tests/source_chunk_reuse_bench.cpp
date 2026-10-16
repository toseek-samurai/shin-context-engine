#include "source_syntax.h"
#include "context_internal.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <list>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

namespace {
using namespace shin::context;
using Clock = std::chrono::steady_clock;

constexpr std::string_view kParserCacheVersion = "source-syntax-cache-experiment.v1";

void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

uint64_t fingerprint(std::string_view bytes) noexcept {
    uint64_t value = 14695981039346656037ULL;
    for (const unsigned char byte : bytes) {
        value ^= byte;
        value *= 1099511628211ULL;
    }
    return value;
}

std::size_t retained_size(const std::string& scope, const std::string& parser_version,
                          const std::string& extension, const std::string& source,
                          const SyntaxChunking& plan) noexcept {
    return sizeof(SyntaxChunking) + 4 * sizeof(std::string) + 3 * sizeof(std::size_t) +
           scope.capacity() + parser_version.capacity() + extension.capacity() + source.capacity() +
           plan.chunks.capacity() * sizeof(SyntaxChunk);
}

bool same_plan(const SyntaxChunking& left, const SyntaxChunking& right,
               std::string_view left_source, std::string_view right_source) {
    if (left.supported != right.supported || left.syntax_complete != right.syntax_complete ||
        left.coverage_complete != right.coverage_complete || left.visited_nodes != right.visited_nodes ||
        left.chunks.size() != right.chunks.size()) return false;
    for (std::size_t i = 0; i < left.chunks.size(); ++i) {
        const auto& a = left.chunks[i];
        const auto& b = right.chunks[i];
        if (a.begin != b.begin || a.end != b.end || a.fallback != b.fallback ||
            a.end > left_source.size() || b.end > right_source.size() ||
            left_source.substr(a.begin, a.end - a.begin) != right_source.substr(b.begin, b.end - b.begin))
            return false;
    }
    return true;
}

struct Request {
    std::string scope;
    std::string extension;
    std::size_t max_bytes = 1024;
    std::size_t max_chunks = 4096;
};

class ExperimentalChunkCache {
public:
    using Hasher = std::function<uint64_t(std::string_view)>;
    explicit ExperimentalChunkCache(std::size_t byte_limit, Hasher hasher = fingerprint)
        : byte_limit_(byte_limit), hasher_(std::move(hasher)) {}

    void set_context(std::string root_project_scope, std::string parser_version) {
        if (scope_ == root_project_scope && parser_version_ == parser_version) return;
        clear();
        scope_ = std::move(root_project_scope);
        parser_version_ = std::move(parser_version);
        ++invalidations_;
    }

    SyntaxChunking get(std::string_view source, const Request& request) {
        const auto request_start = Clock::now();
        require(request.scope == scope_, "request escaped current root/project scope");
        const auto finish = [&](SyntaxChunking plan) -> SyntaxChunking {
            access_ms_ += std::chrono::duration<double, std::milli>(Clock::now() - request_start).count();
            return plan;
        };
        const auto hash = hasher_(source);
        auto bucket = by_fingerprint_.find(hash);
        if (bucket != by_fingerprint_.end()) {
            for (auto position : bucket->second) {
                const Entry& entry = *position;
                if (entry.scope == request.scope && entry.parser_version == parser_version_ &&
                    entry.extension == request.extension && entry.max_bytes == request.max_bytes &&
                    entry.max_chunks == request.max_chunks && entry.source.size() == source.size() &&
                    entry.source == source) {
                    ++hits_;
                    lru_.splice(lru_.begin(), lru_, position);
                    return finish(lru_.front().plan);
                }
            }
        }

        ++misses_;
        const auto start = Clock::now();
        auto plan = chunk_source_syntax(source, request.extension, request.max_bytes, request.max_chunks);
        const auto elapsed = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
        parse_ms_ += elapsed;
        ++parses_;

        Entry entry{request.scope, parser_version_, request.extension, std::string(source),
                    request.max_bytes, request.max_chunks, std::move(plan), 0};
        entry.bytes = retained_size(entry.scope, entry.parser_version, entry.extension,
                                    entry.source, entry.plan);
        if (entry.bytes <= byte_limit_) {
            lru_.push_front(std::move(entry));
            by_fingerprint_[hash].push_back(lru_.begin());
            retained_bytes_ += lru_.front().bytes;
            while (retained_bytes_ > byte_limit_) evict_oldest();
            // An entry within the cap is newer than every eviction candidate.
            return finish(lru_.front().plan);
        }
        return finish(std::move(entry.plan));
    }

    std::size_t hits() const noexcept { return hits_; }
    std::size_t misses() const noexcept { return misses_; }
    std::size_t parses() const noexcept { return parses_; }
    std::size_t evictions() const noexcept { return evictions_; }
    std::size_t invalidations() const noexcept { return invalidations_; }
    std::size_t retained_bytes() const noexcept { return retained_bytes_; }
    double parse_ms() const noexcept { return parse_ms_; }
    double access_ms() const noexcept { return access_ms_; }

private:
    struct Entry {
        std::string scope;
        std::string parser_version;
        std::string extension;
        std::string source;
        std::size_t max_bytes;
        std::size_t max_chunks;
        SyntaxChunking plan;
        std::size_t bytes;
    };

    using Position = std::list<Entry>::iterator;

    void clear() {
        lru_.clear();
        by_fingerprint_.clear();
        retained_bytes_ = 0;
    }

    void evict_oldest() {
        auto last = std::prev(lru_.end());
        const uint64_t hash = hasher_(last->source);
        auto bucket = by_fingerprint_.find(hash);
        if (bucket != by_fingerprint_.end()) {
            auto& entries = bucket->second;
            entries.erase(std::remove(entries.begin(), entries.end(), last), entries.end());
            if (entries.empty()) by_fingerprint_.erase(bucket);
        }
        retained_bytes_ -= last->bytes;
        lru_.erase(last);
        ++evictions_;
    }

    std::size_t byte_limit_;
    Hasher hasher_;
    std::string scope_;
    std::string parser_version_;
    std::list<Entry> lru_;
    std::unordered_map<uint64_t, std::vector<Position>> by_fingerprint_;
    std::size_t retained_bytes_ = 0;
    std::size_t hits_ = 0;
    std::size_t misses_ = 0;
    std::size_t parses_ = 0;
    std::size_t evictions_ = 0;
    std::size_t invalidations_ = 0;
    double parse_ms_ = 0;
    double access_ms_ = 0;
};

double uncached_ms(std::string_view source, const Request& request, SyntaxChunking& result) {
    const auto start = Clock::now();
    result = chunk_source_syntax(source, request.extension, request.max_bytes, request.max_chunks);
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

void compare(const SyntaxChunking& cached, std::string_view cached_source,
             std::string_view extension, std::size_t max_bytes, std::size_t max_chunks) {
    const auto direct = chunk_source_syntax(cached_source, extension, max_bytes, max_chunks);
    require(same_plan(cached, direct, cached_source, cached_source), "cached chunk plan differs from direct parse");
}

std::vector<std::string> verify_selected_files(
    const std::map<std::string, std::string>& files,
    ExperimentalChunkCache& cache,
    const std::string& scope) {
    std::vector<std::string> selected;
    for (const auto& [path, source] : files) {
        const Request request{scope, std::filesystem::path(path).extension().string(), 48, 64};
        const auto cached = cache.get(source, request);
        const auto direct = chunk_source_syntax(source, request.extension, request.max_bytes, request.max_chunks);
        require(same_plan(cached, direct, source, source), "selected-file chunk plan differs from direct computation");
        selected.push_back(path);
    }
    return selected;
}

struct CorpusMetrics {
    std::size_t files = 0;
    std::size_t input_bytes = 0;
    std::size_t unchanged_input_bytes = 0;
    double uncached_ms = 0;
    double cold_cache_ms = 0;
    double cold_parse_ms = 0;
    double unchanged_cache_ms = 0;
    double unchanged_parse_ms = 0;
    std::size_t cold_cache_hits = 0;
    std::size_t cold_cache_misses = 0;
    std::size_t cold_cache_parses = 0;
    std::size_t cold_cache_evictions = 0;
    std::size_t cold_retained_bytes = 0;
    std::size_t unchanged_cache_hits = 0;
    std::size_t unchanged_cache_misses = 0;
    std::size_t unchanged_cache_parses = 0;
    std::size_t unchanged_cache_evictions = 0;
    std::size_t unchanged_retained_bytes = 0;
};

bool is_within(const std::filesystem::path& root, const std::filesystem::path& path) {
    const auto relative = path.lexically_relative(root);
    return !relative.empty() && *relative.begin() != ".." && !relative.is_absolute();
}

std::string read_bounded_file(const std::filesystem::path& path, std::size_t max_bytes,
                             std::uintmax_t expected_size) {
    require(expected_size <= max_bytes, "file exceeds bounded read size");
    std::ifstream input(path, std::ios::binary);
    require(input.is_open(), "could not open bounded input file");
    std::string contents;
    std::array<char, 65536> buffer{};
    while (true) {
        const auto remaining_with_probe = max_bytes + 1 - contents.size();
        const auto request = std::min(buffer.size(), remaining_with_probe);
        input.read(buffer.data(), static_cast<std::streamsize>(request));
        const auto count = input.gcount();
        if (count > 0) {
            contents.append(buffer.data(), static_cast<std::size_t>(count));
            require(contents.size() <= max_bytes, "file grew beyond bounded read size");
        }
        if (input.eof()) break;
        require(input.good() && !input.bad(), "bounded file read failed");
    }
    require(input.eof() && !input.bad() && contents.size() == expected_size,
            "file size changed or bounded read was incomplete");
    return contents;
}

struct SourceClosure {
    std::filesystem::path path;
    std::string extension;
    std::size_t size = 0;
    uint64_t content_hash = 0;
};

CorpusMetrics run_explicit_manifest(const std::filesystem::path& root_path,
                                    const std::filesystem::path& manifest_path) {
    namespace fs = std::filesystem;
    const auto root = fs::canonical(root_path);
    require(fs::is_regular_file(manifest_path) && fs::file_size(manifest_path) <= 256 * 1024,
            "manifest must be a bounded regular file");
    const auto manifest_bytes = read_bounded_file(manifest_path, 256 * 1024, fs::file_size(manifest_path));
    const auto manifest = nlohmann::json::parse(manifest_bytes);
    require(manifest.is_array() && !manifest.empty() && manifest.size() <= 4096,
            "manifest must be a nonempty array of at most 4096 relative source paths");

    ExperimentalChunkCache cache(64 * 1024 * 1024);
    cache.set_context(root.generic_string(), std::string(kParserCacheVersion));
    CorpusMetrics metrics;
    std::vector<SourceClosure> closure;
    closure.reserve(manifest.size());
    for (const auto& item : manifest) {
        require(item.is_string(), "manifest entries must be source paths");
        const fs::path relative(item.get<std::string>());
        require(!relative.empty() && !relative.is_absolute(), "manifest path must be relative");
        for (const auto& component : relative)
            require(component != ".." && component != ".", "manifest path traversal refused");
        const auto path = fs::canonical(root / relative);
        require(is_within(root, path) && fs::is_regular_file(path), "manifest source must be a regular file under root");
        const auto size = fs::file_size(path);
        require(size <= 1024 * 1024 && metrics.input_bytes <= 64 * 1024 * 1024 - size,
                "explicit source corpus exceeds bounded input size");
        std::string source = read_bounded_file(path, 1024 * 1024, size);
        metrics.input_bytes += source.size();
        ++metrics.files;
        closure.push_back({path, path.extension().string(), source.size(), fingerprint(source)});

        const Request request{root.generic_string(), closure.back().extension, 1024, 4096};
        SyntaxChunking direct;
        metrics.uncached_ms += uncached_ms(source, request, direct);
        const auto access_start = Clock::now();
        const auto& cold = cache.get(source, request);
        metrics.cold_cache_ms += std::chrono::duration<double, std::milli>(Clock::now() - access_start).count();
        require(same_plan(cold, direct, source, source), "cold corpus plan differs from direct parse");
    }
    metrics.cold_cache_hits = cache.hits();
    metrics.cold_cache_misses = cache.misses();
    metrics.cold_cache_parses = cache.parses();
    metrics.cold_cache_evictions = cache.evictions();
    metrics.cold_retained_bytes = cache.retained_bytes();
    metrics.cold_parse_ms = cache.parse_ms();

    const auto cold_access_ms = metrics.cold_cache_ms;
    metrics.cold_cache_ms = 0;
    for (std::size_t index = 0; index < manifest.size(); ++index) {
        const auto& item = manifest[index];
        const fs::path relative(item.get<std::string>());
        const auto path = fs::canonical(root / relative);
        require(is_within(root, path) && fs::is_regular_file(path), "manifest source moved outside root");
        const auto size = fs::file_size(path);
        require(size <= 1024 * 1024 && metrics.unchanged_input_bytes <= 64 * 1024 * 1024 - size,
                "unchanged source corpus exceeds bounded input size");
        std::string source = read_bounded_file(path, 1024 * 1024, size);
        metrics.unchanged_input_bytes += source.size();
        require(path == closure[index].path && path.extension().string() == closure[index].extension &&
                source.size() == closure[index].size && fingerprint(source) == closure[index].content_hash,
                "explicit source changed between passes");
        const Request request{root.generic_string(), closure[index].extension, 1024, 4096};
        SyntaxChunking direct;
        (void)uncached_ms(source, request, direct); // Oracle is intentionally outside cache timing.
        const auto access_start = Clock::now();
        const auto& hit = cache.get(source, request);
        metrics.unchanged_cache_ms += std::chrono::duration<double, std::milli>(Clock::now() - access_start).count();
        require(same_plan(hit, direct, source, source), "unchanged corpus plan differs from direct parse");
    }
    metrics.cold_cache_ms = cold_access_ms;
    metrics.unchanged_cache_hits = cache.hits() - metrics.cold_cache_hits;
    metrics.unchanged_cache_parses = cache.parses() - metrics.cold_cache_parses;
    metrics.unchanged_cache_misses = cache.misses() - metrics.cold_cache_misses;
    metrics.unchanged_cache_evictions = cache.evictions() - metrics.cold_cache_evictions;
    metrics.unchanged_retained_bytes = cache.retained_bytes();
    metrics.unchanged_parse_ms = cache.parse_ms() - metrics.cold_parse_ms;
    require(metrics.unchanged_input_bytes == metrics.input_bytes,
            "source corpus byte count changed between passes");
    return metrics;
}

void exercise() {
    const std::string root_a = "synthetic-root-a/project-1";
    const Request cpp{root_a, ".cpp", 48, 64};
    const std::string source =
        "#include <string>\nclass Account {\n public:\n  int balance() const { return 7; }\n};\n";
    const std::string edited =
        "#include <string>\nclass Account {\n public:\n  int balance() const { return 8; }\n};\n";
    ExperimentalChunkCache cache(64 * 1024);
    cache.set_context(root_a, std::string(kParserCacheVersion));

    SyntaxChunking direct;
    const double cold_direct_ms = uncached_ms(source, cpp, direct);
    const auto& cold = cache.get(source, cpp);
    require(same_plan(cold, direct, source, source), "cold plan differs from independent computation");
    const auto cold_parse_ms = cache.parse_ms();

    const double unchanged_direct_ms = uncached_ms(source, cpp, direct);
    const auto& unchanged = cache.get(source, cpp);
    require(same_plan(unchanged, direct, source, source), "unchanged hit differs from independent computation");
    require(cache.hits() == 1 && cache.parses() == 1, "unchanged source was parsed again");

    std::map<std::string, std::string> project_files{{"Account.cpp", source}};
    auto renamed_content = std::move(project_files.at("Account.cpp"));
    project_files.erase("Account.cpp");
    project_files.emplace("Ledger.cpp", renamed_content);
    const Request renamed = cpp; // The path changes; chunking identity uses exact bytes and extension.
    const auto& rename_hit = cache.get(project_files.at("Ledger.cpp"), renamed);
    require(same_plan(rename_hit, direct, source, source), "rename reuse differs from independent computation");
    require(cache.hits() == 2 && cache.parses() == 1, "rename did not reuse identical source bytes");

    const double edit_direct_ms = uncached_ms(edited, cpp, direct);
    const auto& edit = cache.get(edited, cpp);
    require(same_plan(edit, direct, edited, edited), "edited source differs from independent computation");
    require(cache.misses() == 2 && cache.parses() == 2, "one-file edit reused stale chunk plan");

    project_files.emplace("Stale.cpp", edited);
    project_files.erase("Stale.cpp"); // Deletion removes the source from the caller's file state.
    require(project_files.size() == 1 && !project_files.contains("Stale.cpp"), "delete scenario failed");
    const auto selected_after_delete = verify_selected_files(project_files, cache, root_a);
    require(selected_after_delete == std::vector<std::string>{"Ledger.cpp"} &&
            std::find(selected_after_delete.begin(), selected_after_delete.end(), "Stale.cpp") == selected_after_delete.end(),
            "deleted path leaked into current source selection");
    const auto retained_deleted_plan = cache.get(edited, cpp);
    const auto direct_deleted_plan = chunk_source_syntax(edited, cpp.extension, cpp.max_bytes, cpp.max_chunks);
    require(same_plan(retained_deleted_plan, direct_deleted_plan, edited, edited),
            "deleted source cache entry is not a reusable exact plan");

    const Request other_extension{root_a, ".hpp", 48, 64};
    const auto& extension_plan = cache.get(edited, other_extension);
    compare(extension_plan, edited, other_extension.extension, other_extension.max_bytes,
            other_extension.max_chunks);
    const Request other_budget{root_a, ".cpp", 32, 64};
    const auto& budget_plan = cache.get(edited, other_budget);
    compare(budget_plan, edited, other_budget.extension, other_budget.max_bytes, other_budget.max_chunks);
    const Request other_chunk_cap{root_a, ".cpp", 32, 1};
    const auto before_chunk_cap_parse = cache.parses();
    const auto& chunk_cap_plan = cache.get(edited, other_chunk_cap);
    compare(chunk_cap_plan, edited, other_chunk_cap.extension, other_chunk_cap.max_bytes,
            other_chunk_cap.max_chunks);
    require(cache.parses() == before_chunk_cap_parse + 1, "max_chunks change did not miss the cache");

    ExperimentalChunkCache collision_cache(64 * 1024,
        [](std::string_view) { return uint64_t{7}; });
    collision_cache.set_context(root_a, std::string(kParserCacheVersion));
    const std::string collision_variant = "class Different { int total() { return 123456; } };\n";
    require(collision_variant.size() != source.size(), "collision fixture must vary source length");
    const auto collision_first = collision_cache.get(source, cpp);
    const auto collision_second = collision_cache.get(collision_variant, cpp);
    const auto collision_repeat = collision_cache.get(source, cpp);
    const auto direct_first = chunk_source_syntax(source, cpp.extension, cpp.max_bytes, cpp.max_chunks);
    const auto direct_second = chunk_source_syntax(collision_variant, cpp.extension, cpp.max_bytes, cpp.max_chunks);
    require(same_plan(collision_first, direct_first, source, source) &&
            same_plan(collision_second, direct_second, collision_variant, collision_variant) &&
            same_plan(collision_repeat, direct_first, source, source),
            "forced fingerprint collision returned another source plan");
    require(collision_cache.hits() == 1 && collision_cache.misses() == 2 && collision_cache.parses() == 2,
            "forced fingerprint collision did not require full source equality");

    ExperimentalChunkCache oversized_cache(1);
    oversized_cache.set_context(root_a, std::string(kParserCacheVersion));
    const auto oversized_first = oversized_cache.get(source, cpp);
    const auto oversized_second = oversized_cache.get(source, cpp);
    compare(oversized_first, source, cpp.extension, cpp.max_bytes, cpp.max_chunks);
    compare(oversized_second, source, cpp.extension, cpp.max_bytes, cpp.max_chunks);
    require(oversized_cache.retained_bytes() == 0 && oversized_cache.hits() == 0 &&
            oversized_cache.parses() == 2, "oversized entry was retained or reused");

    ExperimentalChunkCache zero_budget_cache(0);
    zero_budget_cache.set_context(root_a, std::string(kParserCacheVersion));
    const auto zero_first = zero_budget_cache.get(source, cpp);
    const auto zero_second = zero_budget_cache.get(source, cpp);
    compare(zero_first, source, cpp.extension, cpp.max_bytes, cpp.max_chunks);
    compare(zero_second, source, cpp.extension, cpp.max_bytes, cpp.max_chunks);
    require(zero_budget_cache.retained_bytes() == 0 && zero_budget_cache.hits() == 0 &&
            zero_budget_cache.parses() == 2, "zero byte budget retained or reused an entry");

    cache.set_context(root_a, "source-syntax-cache-experiment.v2");
    const auto before_version_parse = cache.parses();
    const auto& version_plan = cache.get(source, cpp);
    compare(version_plan, source, cpp.extension, cpp.max_bytes, cpp.max_chunks);
    require(cache.parses() == before_version_parse + 1, "parser version did not invalidate cache");

    const auto other_root = "synthetic-root-b/project-1";
    cache.set_context(other_root, "source-syntax-cache-experiment.v2");
    const Request other_scope{other_root, ".cpp", 48, 64};
    const auto before_root_parse = cache.parses();
    const auto& root_plan = cache.get(source, other_scope);
    compare(root_plan, source, other_scope.extension, other_scope.max_bytes, other_scope.max_chunks);
    require(cache.parses() == before_root_parse + 1, "root/project scope change reused cache");

    ExperimentalChunkCache small(1200);
    small.set_context(root_a, std::string(kParserCacheVersion));
    std::vector<std::string> eviction_sources;
    for (int index = 0; index < 8; ++index) {
        eviction_sources.push_back("// synthetic " + std::to_string(index) + "\n" +
            "int value_" + std::to_string(index) + "() { return " + std::to_string(index) + "; }\n");
        Request request{root_a, ".cpp", 32, 32};
        const auto& plan = small.get(eviction_sources.back(), request);
        compare(plan, eviction_sources.back(), request.extension, request.max_bytes, request.max_chunks);
    }
    require(small.retained_bytes() <= 1200 && small.evictions() > 0, "LRU memory bound was not enforced");
    const auto before_evicted_parse = small.parses();
    Request eviction_request{root_a, ".cpp", 32, 32};
    const auto& reloaded = small.get(eviction_sources.front(), eviction_request);
    compare(reloaded, eviction_sources.front(), eviction_request.extension,
            eviction_request.max_bytes, eviction_request.max_chunks);
    require(small.parses() == before_evicted_parse + 1, "evicted plan was unexpectedly retained");

    std::cout << "{\"schema_version\":\"shin.source-chunk-reuse-bench.v1\","
              << "\"ok\":true,\"source_mutated\":false,\"model_runs\":0,"
              << "\"synthetic_cases\":{\"cold\":1,\"unchanged\":1,\"one_file_edit\":1,"
              << "\"rename\":1,\"delete\":1,\"extension_change\":1,\"budget_change\":1,"
              << "\"max_chunks_change\":1,\"forced_hash_collision\":1,\"oversized_uncached\":1,"
              << "\"zero_budget\":1,\"parser_version_invalidation\":1,"
              << "\"root_project_invalidation\":1,\"eviction\":8},"
              << "\"timing_ms\":{\"cold_uncached\":" << cold_direct_ms
              << ",\"cold_cached_parse\":" << cold_parse_ms
              << ",\"unchanged_uncached\":" << unchanged_direct_ms
              << ",\"one_file_edit_uncached\":" << edit_direct_ms << "},"
              << "\"cache\":{\"hits\":" << cache.hits() << ",\"misses\":" << cache.misses()
              << ",\"parses\":" << cache.parses() << ",\"evictions\":" << cache.evictions()
              << ",\"invalidations\":" << cache.invalidations()
              << ",\"retained_payload_bytes_estimate\":" << cache.retained_bytes()
              << ",\"parse_ms\":" << cache.parse_ms()
              << ",\"cache_access_ms\":" << cache.access_ms() << "},"
              << "\"eviction_cache\":{\"hits\":" << small.hits() << ",\"misses\":" << small.misses()
              << ",\"parses\":" << small.parses() << ",\"evictions\":" << small.evictions()
              << ",\"retained_payload_bytes_estimate\":" << small.retained_bytes()
              << ",\"byte_limit\":1200},"
              << "\"process_peak_rss_bytes\":" << shin::context::process_peak_rss_bytes() << ","
              << "\"boundary\":\"Deterministic synthetic source only. Retained payload is an estimate excluding container allocator overhead, not an RSS cap. Delete correctness checks caller source selection; a reusable plan may remain cached until eviction. Production discovery chunking_ms also includes source_line_offsets; this probe times chunk_source_syntax and cache access only. Source scans, offsets, path metadata and token projections remain separate costs. Freshness, rights, and source admission are never cached.\"}\n";
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 1 && argc != 3)
            throw std::runtime_error("usage: shin-source-chunk-reuse-bench [EXPLICIT_SOURCE_ROOT JSON_PATH_MANIFEST]");
        if (argc == 1) {
            exercise();
            return 0;
        }
        if (argc == 3) {
            const auto corpus = run_explicit_manifest(argv[1], argv[2]);
            std::cout << "{\"schema_version\":\"shin.source-chunk-reuse-corpus.v1\",\"ok\":true,"
                      << "\"source_mutated\":false,\"model_runs\":0,\"files\":" << corpus.files
                      << ",\"input_bytes\":" << corpus.input_bytes
                      << ",\"timing_ms\":{\"uncached_oracle\":" << corpus.uncached_ms
                      << ",\"cold_cache_access\":" << corpus.cold_cache_ms
                      << ",\"cold_parse\":" << corpus.cold_parse_ms
                      << ",\"unchanged_cache_access\":" << corpus.unchanged_cache_ms
                      << ",\"unchanged_parse\":" << corpus.unchanged_parse_ms << "},"
                      << "\"cold_cache\":{\"hits\":" << corpus.cold_cache_hits
                      << ",\"misses\":" << corpus.cold_cache_misses
                      << ",\"parses\":" << corpus.cold_cache_parses
                      << ",\"evictions\":" << corpus.cold_cache_evictions
                      << ",\"retained_payload_bytes_estimate\":" << corpus.cold_retained_bytes << "},"
                      << "\"unchanged_cache\":{\"hits\":" << corpus.unchanged_cache_hits
                      << ",\"misses\":" << corpus.unchanged_cache_misses
                      << ",\"parses\":" << corpus.unchanged_cache_parses
                      << ",\"evictions\":" << corpus.unchanged_cache_evictions
                      << ",\"retained_payload_bytes_estimate\":" << corpus.unchanged_retained_bytes << "},"
                      << "\"process_peak_rss_bytes\":" << shin::context::process_peak_rss_bytes() << ","
                      << "\"boundary\":\"Only explicitly listed files under the supplied root are read; bounded source reads and a non-cryptographic FNV between-pass diagnostic are separate from cache behavior. Cache hits verify exact source bytes. Frozen corpus identity requires an external before/after SHA-256 check by the measurement driver. Source reads and direct oracle comparisons are outside cache-access timing. Retained payload is an estimate that excludes container allocator overhead and is not an RSS cap. Rename reuse applies to SyntaxChunking only; metadata, path-bound fields, and token projections must be rebuilt. Freshness and rights are never cached.\"}\n";
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        std::cout << "{\"schema_version\":\"shin.source-chunk-reuse-bench.v1\",\"ok\":false}\n";
        return 1;
    }
}
