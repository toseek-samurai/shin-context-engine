#include "source_discovery.h"
#include "context_internal.h"
#include "generated_io.h"
#include "generated_snapshot.h"
#include "source_read.h"
#include "source_syntax.h"
#include "source_cache.h"
#include "source_context.h"
#include "source_query_terms.h"
#include "generation_admission.h"

#include <sqlite3.h>
#include <mutex>

namespace shin::context {
namespace {
constexpr std::size_t kFileBytes = 1024 * 1024;
constexpr std::size_t kSourceBytes = 64 * 1024 * 1024;
constexpr std::size_t kRows = 65536;
using Database = std::unique_ptr<sqlite3, decltype(&sqlite3_close)>;
using Statement = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;

void require(bool condition, const char* code) {
    if (!condition) throw CliError(code, "source-discovery", "Source discovery refused incomplete or over-budget input.",
                                   1, false, "Check the explicit source profile and discovery limits.");
}

Statement prepare(sqlite3* database, const char* sql) {
    sqlite3_stmt* raw = nullptr;
    const int rc = sqlite3_prepare_v2(database, sql, -1, &raw, nullptr);
    Statement result(raw, sqlite3_finalize);
    require(rc == SQLITE_OK, "source-discovery-sql");
    return result;
}

void execute(sqlite3* database, const char* sql) {
    require(sqlite3_exec(database, sql, nullptr, nullptr, nullptr) == SQLITE_OK, "source-discovery-sql");
}

void bind_text(Statement& statement, int column, const std::string& value) {
    require(sqlite3_bind_text(statement.get(), column, value.data(), static_cast<int>(value.size()), SQLITE_TRANSIENT) == SQLITE_OK,
            "source-discovery-sql");
}

std::string column_text(sqlite3_stmt* statement, int column) {
    const auto* data = sqlite3_column_text(statement, column);
    return data ? std::string(reinterpret_cast<const char*>(data), static_cast<std::size_t>(sqlite3_column_bytes(statement, column))) : "";
}
void validate_queries(const std::vector<std::string>& queries) {
    require(!queries.empty() && queries.size() <= 32, "source-discovery-query-limit");
    for (const auto& query : queries)
        require(!query.empty() && query.size() <= 4096 && query.find('\0') == std::string::npos,
                "source-discovery-query-limit");
}
}  // namespace

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

struct SourceDiscoveryIndex::Impl {
    Impl(RuntimeOptions value, const fs::path& cache_directory);
    Json query(const RuntimeOptions& binding, const std::vector<std::string>& queries,
               bool materialize = false, bool at_snapshot = false);
    void persist_published_cache(const RuntimeOptions& destination, const fs::path& cache_directory);
    Json preparation_metrics() const;
    void configure(sqlite3* db);
    bool restore(const std::string& bytes, const Json& binding, const Json& schema,
                 const std::map<std::string, std::string>& sources);
    void persist(SourceCache& cache, const Json& binding);
    IndexFreshness validate_current();
    void check_time() const { require(SteadyClock::now() < deadline, "source-discovery-timeout"); }
    void check_profile() const {
        if (!options.profile.config_source.empty()) {
            const auto path = fs::absolute(options.profile.config_source).lexically_normal();
            (void)read_bound_source(path.parent_path(), path.filename().generic_string(), profile_hash, kFileBytes);
        }
    }

    RuntimeOptions options;
    std::string profile_hash;
    std::string snapshot_id;
    mutable std::mutex mutex;
    SteadyClock::time_point deadline;
    Database database{nullptr, sqlite3_close};
    std::size_t chunks = 0;
    std::map<std::string, std::string> sources;
    std::size_t files = 0, total_bytes = 0, redacted_files = 0, fallback_chunks = 0;
    std::size_t indexed_text_bytes = 0;
    std::size_t successful_queries = 0;
    double preparation_ms = 0, read_scan_ms = 0, chunking_ms = 0, sql_index_ms = 0, persist_ms = 0;
    double transfer_ms = 0, transfer_verify_ms = 0, transfer_snapshot_ms = 0;
    bool invalidated = false;
    bool persisted_cache = false;
    std::string cache_status = "disabled";
};

namespace {
Json database_schema(sqlite3* db) {
    auto query = prepare(db, "SELECT type,name,tbl_name,sql FROM sqlite_schema ORDER BY name");
    Json rows = Json::array();
    int rc;
    while ((rc = sqlite3_step(query.get())) == SQLITE_ROW) {
        require(rows.size() < 32, "source-cache-invalid");
        rows.push_back({column_text(query.get(), 0), column_text(query.get(), 1),
                        column_text(query.get(), 2), column_text(query.get(), 3)});
    }
    require(rc == SQLITE_DONE, "source-cache-invalid");
    return rows;
}

Json source_cache_identity(const RuntimeOptions& options) {
    return {{"root", options.root.generic_string()}, {"index", options.out_root.generic_string()},
            {"profile", options.profile.id}, {"profile_source", options.profile.config_source}};
}

Json source_cache_binding(const RuntimeOptions& options, const std::string& profile_hash,
                          const std::string& snapshot_id) {
    return {{"identity", source_cache_identity(options)}, {"profile_hash", profile_hash}, {"snapshot", snapshot_id},
            {"format", "shin.source-cache.v3"}, {"engine", kProductVersion}, {"sqlite", sqlite3_libversion()}};
}

std::map<std::string, std::string> source_bindings(const RetrievalSession& session) {
    std::map<std::string, std::string> result;
    for (const auto& source : session.manifest().at("source_fact_sources")) {
        if (source.at("status") != "scanned") continue;
        require(result.size() < 10000, "source-discovery-file-limit");
        require(result.emplace(source.at("path"), source.at("content_hash")).second,
                "source-discovery-duplicate-path");
    }
    const auto& counts = session.manifest().at("source_fact_counts");
    require(json_int_value(counts, "capped_files", -1) == 0 &&
            json_int_value(counts, "skipped_large_files", -1) == 0 &&
            json_int_value(counts, "scanned_files", -1) == json_int_value(counts, "candidate_files", -2) &&
            result.size() == static_cast<std::size_t>(json_int_value(counts, "scanned_files", -1)),
            "source-discovery-incomplete-source");
    return result;
}
}

void SourceDiscoveryIndex::Impl::configure(sqlite3* db) {
    // SQLite owns the temporary file and spills pages instead of retaining the
    // complete FTS image. This is a page-cache target, not a total RSS promise.
    execute(db, "PRAGMA cache_size=-4096; PRAGMA mmap_size=0; PRAGMA temp_store=FILE");
    sqlite3_limit(db, SQLITE_LIMIT_LENGTH, 16384);
    sqlite3_limit(db, SQLITE_LIMIT_SQL_LENGTH, 16384);
    require(sqlite3_db_config(db, SQLITE_DBCONFIG_DEFENSIVE, 1, nullptr) == SQLITE_OK &&
            sqlite3_db_config(db, SQLITE_DBCONFIG_TRUSTED_SCHEMA, 0, nullptr) == SQLITE_OK, "source-discovery-sql");
    sqlite3_progress_handler(db, 1000, [](void* pointer) {
        return SteadyClock::now() >= *static_cast<const SteadyClock::time_point*>(pointer) ? 1 : 0;
    }, &deadline);
    // Query vocabulary must never enter a serialized/published source cache,
    // including a prepared generation transferred after a validation query.
    execute(db, "CREATE VIRTUAL TABLE temp.query_text USING fts5(body, tokenize='unicode61');"
                "CREATE VIRTUAL TABLE temp.query_vocab USING fts5vocab(query_text, 'row');"
                "CREATE VIRTUAL TABLE temp.query_stems USING fts5(body, tokenize='porter unicode61');"
                "CREATE VIRTUAL TABLE temp.stem_vocab USING fts5vocab(query_stems, 'instance');");
}

bool SourceDiscoveryIndex::Impl::restore(const std::string& bytes, const Json& binding, const Json& schema,
                                         const std::map<std::string, std::string>& sources) {
    if (bytes.empty()) return false;
    sqlite3* raw = nullptr;
    const auto opened = sqlite3_open_v2("", &raw, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX, nullptr);
    Database loaded(raw, sqlite3_close);
    require(opened == SQLITE_OK, "source-discovery-sql");
    configure(raw);
    sqlite3* input = nullptr;
    const auto input_opened = sqlite3_open_v2(":memory:", &input, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX, nullptr);
    Database image(input, sqlite3_close);
    require(input_opened == SQLITE_OK, "source-discovery-sql");
    // READONLY borrows the bounded verified cache bytes only during backup.
    if (sqlite3_deserialize(input, "main", reinterpret_cast<unsigned char*>(const_cast<char*>(bytes.data())),
                            static_cast<sqlite3_int64>(bytes.size()), static_cast<sqlite3_int64>(bytes.size()),
                            SQLITE_DESERIALIZE_READONLY) != SQLITE_OK) return false;
    sqlite3_backup* copy = sqlite3_backup_init(raw, "main", input, "main");
    if (!copy) return false;
    int copied;
    do { copied = sqlite3_backup_step(copy, 256); }
    while (copied == SQLITE_OK && SteadyClock::now() < deadline);
    const int finished = sqlite3_backup_finish(copy);
    if (copied != SQLITE_DONE || finished != SQLITE_OK) return false;
    image.reset();
    try {
        require(database_schema(raw) == schema, "source-cache-schema");
        auto health = prepare(raw, "PRAGMA quick_check(1)");
        require(sqlite3_step(health.get()) == SQLITE_ROW && column_text(health.get(), 0) == "ok" &&
                sqlite3_step(health.get()) == SQLITE_DONE, "source-cache-integrity");
        execute(raw, "INSERT INTO chunks(chunks) VALUES('integrity-check')");
        auto header = prepare(raw, "SELECT payload FROM cache_meta WHERE id=0");
        require(sqlite3_step(header.get()) == SQLITE_ROW, "source-cache-binding");
        const auto saved = Json::parse(column_text(header.get(), 0));
        require(saved.at("binding") == binding, "source-cache-binding");
        auto records = prepare(raw, "SELECT id,payload FROM cache_meta WHERE id>0 ORDER BY id");
        std::size_t restored = 0;
        int rc;
        while ((rc = sqlite3_step(records.get())) == SQLITE_ROW) {
            require(restored < kRows && sqlite3_column_int64(records.get(), 0) ==
                    static_cast<sqlite3_int64>(restored) + 1, "source-cache-records");
            auto row = Json::parse(column_text(records.get(), 1));
            require(row.is_object() && row.size() == 8 && row.at("path").is_string() &&
                    row.at("source_content_hash").is_string() && row.at("span_hash").is_string() &&
                    row.at("range_origin").is_string() && row.at("byte_start").is_number_unsigned() &&
                    row.at("byte_end").is_number_unsigned() && row.at("start_line").is_number_unsigned() &&
                    row.at("end_line").is_number_unsigned(), "source-cache-records");
            const auto source = sources.find(row.at("path").get<std::string>());
            require(source != sources.end() && row.at("source_content_hash") == source->second &&
                    row.at("byte_start").get<std::size_t>() < row.at("byte_end").get<std::size_t>() &&
                    row.at("byte_end").get<std::size_t>() <= kFileBytes &&
                    row.at("start_line").get<std::size_t>() > 0 &&
                    row.at("end_line").get<std::size_t>() >= row.at("start_line").get<std::size_t>(), "source-cache-records");
            ++restored;
        }
        require(rc == SQLITE_DONE && restored == saved.at("chunks"), "source-cache-records");
        auto count = prepare(raw, "SELECT count(*) FROM chunks");
        require(sqlite3_step(count.get()) == SQLITE_ROW && sqlite3_column_int64(count.get(), 0) ==
                static_cast<sqlite3_int64>(restored), "source-cache-records");
        const auto saved_files = saved.at("files").get<std::size_t>();
        const auto saved_bytes = saved.at("source_bytes").get<std::size_t>();
        const auto saved_redacted = saved.at("redacted_files").get<std::size_t>();
        const auto saved_fallback = saved.at("fallback_chunks").get<std::size_t>();
        const auto saved_indexed = saved.at("indexed_text_bytes").get<std::size_t>();
        require(saved_files == sources.size() && saved_bytes <= kSourceBytes && saved_redacted <= saved_files &&
                saved_fallback <= restored && saved_indexed <= SourceCache::maximum_bytes, "source-cache-records");
        execute(raw, "PRAGMA max_page_count=65536");
        files = saved_files; total_bytes = saved_bytes; redacted_files = saved_redacted; fallback_chunks = saved_fallback;
        indexed_text_bytes = saved_indexed;
        chunks = restored;
    } catch (const CliError&) { return false; }
      catch (const Json::exception&) { return false; }
    database = std::move(loaded);
    return true;
}

void SourceDiscoveryIndex::Impl::persist(SourceCache& cache, const Json& binding) {
    auto insert = prepare(database.get(), "INSERT INTO cache_meta(id,payload) VALUES (?,?)");
    const Json header = {{"binding", binding}, {"files", files}, {"source_bytes", total_bytes},
                         {"indexed_text_bytes", indexed_text_bytes},
                         {"redacted_files", redacted_files}, {"fallback_chunks", fallback_chunks}, {"chunks", chunks}};
    execute(database.get(), "BEGIN");
    require(sqlite3_bind_int64(insert.get(), 1, 0) == SQLITE_OK, "source-discovery-sql");
    bind_text(insert, 2, header.dump());
    require(sqlite3_step(insert.get()) == SQLITE_DONE && sqlite3_reset(insert.get()) == SQLITE_OK, "source-discovery-sql");
    execute(database.get(), "COMMIT; INSERT INTO chunks(chunks) VALUES('optimize');"
                            "INSERT INTO chunks(chunks) VALUES('integrity-check');");
    sqlite3_int64 size = 0;
    std::unique_ptr<unsigned char, decltype(&sqlite3_free)> bytes(sqlite3_serialize(database.get(), "main", &size, 0), sqlite3_free);
    require(bytes && size > 0 && static_cast<std::uint64_t>(size) <= SourceCache::maximum_bytes, "source-cache-size");
    check_time();
    cache.save(std::string_view(reinterpret_cast<const char*>(bytes.get()), static_cast<std::size_t>(size)));
}

SourceDiscoveryIndex::Impl::Impl(RuntimeOptions value, const fs::path& cache_directory)
    : options(std::move(value)), profile_hash(profile_config_hash(options.profile)) {
    const auto started = SteadyClock::now();
    require(options.root.is_absolute() && options.out_root.is_absolute() &&
            options.root.lexically_normal() == options.root && options.out_root.lexically_normal() == options.out_root &&
            (options.profile.config_source.empty() || fs::path(options.profile.config_source).is_absolute()),
            "source-discovery-invalid-binding");
    check_profile();
    if (options.generated_snapshot) {
        const auto& retained = options.generated_snapshot->options();
        require(options.root == retained.root && options.out_root == retained.out_root &&
                options.profile.id == retained.profile.id && profile_hash == profile_config_hash(retained.profile) &&
                options.profile.config_source == retained.profile.config_source && !options.force_source_scan,
                "source-discovery-binding-mismatch");
    }
    auto session = [&]() -> RetrievalSession {
        if (options.generated_snapshot)
            return RetrievalSession(options.generated_snapshot, RetrievalSessionOpenMode::verified_snapshot);
        return RetrievalSession(options, RetrievalSessionOpenMode::verified_snapshot);
    }();
    snapshot_id = session.freshness().snapshot_id;
    (void)validate_current();
    const auto& source_counts = session.manifest().at("source_fact_counts");
    deadline = SteadyClock::now() + std::chrono::seconds(30);
    require(sqlite3_threadsafe() != 0, "source-discovery-sqlite-threading");
    sqlite3* raw = nullptr;
    const int opened = sqlite3_open_v2("", &raw, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX, nullptr);
    database.reset(raw);
    require(opened == SQLITE_OK, "source-discovery-sql");
    configure(raw);
    execute(raw, "PRAGMA page_size=4096; PRAGMA max_page_count=65536;"
                 "CREATE VIRTUAL TABLE chunks USING fts5(body, tokenize='porter unicode61');"
                 "CREATE TABLE cache_meta(id INTEGER PRIMARY KEY, payload TEXT NOT NULL);");
    std::unique_ptr<SourceCache> cache;
    sources = source_bindings(session);
    require(sources.size() == static_cast<std::size_t>(json_int_value(source_counts, "scanned_files", -1)),
            "source-discovery-incomplete-source");
    const Json identity = source_cache_identity(options);
    const Json binding = source_cache_binding(options, profile_hash, snapshot_id);
    if (!cache_directory.empty()) {
        cache = std::make_unique<SourceCache>(cache_directory, fnv1a64(identity.dump()).substr(8));
        const auto saved = cache->load();
        cache_status = saved ? "rebuilt" : "created";
        if (saved && restore(*saved, binding, database_schema(raw), sources)) {
            cache_status = "restored";
            persisted_cache = true;
            (void)validate_current();
            check_time();
            preparation_ms = elapsed_ms_since(started);
            return;
        }
    }
    execute(raw, "BEGIN");
    files = sources.size();
    auto insert = prepare(raw, "INSERT INTO chunks(rowid,body) VALUES (?,?)");
    auto record = prepare(raw, "INSERT INTO cache_meta(id,payload) VALUES (?,?)");
    for (const auto& [path, hash] : sources) {
        check_time();
        const auto read_started = SteadyClock::now();
        const auto content = read_bound_source(options.root, path, hash, kFileBytes);
        total_bytes += content.size();
        require(total_bytes <= kSourceBytes, "source-discovery-byte-limit");
        const bool redacted = source_range_secret(content).found;
        read_scan_ms += elapsed_ms_since(read_started);
        if (redacted) { ++redacted_files; continue; }
        const auto chunk_started = SteadyClock::now();
        const auto plan = chunk_source_syntax(content, fs::path(path).extension().string());
        require(plan.coverage_complete, "source-discovery-incomplete-source");
        const auto offsets = source_line_offsets(content);
        chunking_ms += elapsed_ms_since(chunk_started);
        for (const auto& chunk : plan.chunks) {
            require(chunks < kRows, "source-discovery-row-limit");
            const auto text = content.substr(chunk.begin, chunk.end - chunk.begin);
            const Json metadata = {{"path", path}, {"source_content_hash", hash}, {"span_hash", fnv1a64(text)},
                {"byte_start", chunk.begin}, {"byte_end", chunk.end},
                {"start_line", std::upper_bound(offsets.begin(), offsets.end(), chunk.begin) - offsets.begin()},
                {"end_line", std::upper_bound(offsets.begin(), offsets.end(), chunk.end - 1) - offsets.begin()},
                {"range_origin", chunk.fallback ? "bounded-text-fallback" : "syntax-partition"}};
            ++chunks;
            const auto index_started = SteadyClock::now();
            require(sqlite3_bind_int64(record.get(), 1, static_cast<sqlite3_int64>(chunks)) == SQLITE_OK,
                    "source-discovery-sql");
            bind_text(record, 2, metadata.dump());
            require(sqlite3_step(record.get()) == SQLITE_DONE && sqlite3_reset(record.get()) == SQLITE_OK,
                    "source-discovery-sql");
            fallback_chunks += chunk.fallback;
            require(sqlite3_bind_int64(insert.get(), 1, static_cast<sqlite3_int64>(chunks)) == SQLITE_OK,
                    "source-discovery-sql");
            const auto projected = source_search_projection(path + "\n" + text);
            indexed_text_bytes += projected.size();
            require(indexed_text_bytes <= SourceCache::maximum_bytes, "source-discovery-byte-limit");
            bind_text(insert, 2, projected);
            require(sqlite3_step(insert.get()) == SQLITE_DONE, "source-discovery-sql");
            require(sqlite3_reset(insert.get()) == SQLITE_OK, "source-discovery-sql");
            sql_index_ms += elapsed_ms_since(index_started);
        }
    }
    execute(raw, "COMMIT;");
    (void)validate_current();
    if (cache) {
        const auto persist_started = SteadyClock::now();
        persist(*cache, binding);
        persist_ms += elapsed_ms_since(persist_started);
        persisted_cache = true;
        (void)validate_current();
    }
    check_time();
    preparation_ms = elapsed_ms_since(started);
    // Do not retain the publication lock while idle; a writer must be able to
    // publish a new generation, which the next query then refuses to reuse.
}

IndexFreshness SourceDiscoveryIndex::Impl::validate_current() {
    require(!invalidated, "source-discovery-invalidated");
    try {
        check_profile();
        auto freshness = options.generation_admission ? assess_index_freshness(options, true) :
                                                       assess_source_discovery_freshness(options);
        require_current_index_or_throw(freshness);
        require(freshness.snapshot_id == snapshot_id, "source-discovery-snapshot-changed");
        check_profile();
        return freshness;
    } catch (...) {
        invalidated = true;
        throw;
    }
}

void SourceDiscoveryIndex::Impl::persist_published_cache(const RuntimeOptions& destination,
                                                          const fs::path& cache_directory) {
    std::unique_lock request_lock(mutex, std::try_to_lock);
    require(request_lock.owns_lock(), "source-discovery-busy");
    require(!invalidated, "source-discovery-invalidated");
    require(!persisted_cache, "source-cache-already-persisted");
    require(options.generation_admission && destination.generation_admission &&
            options.root == destination.root && options.root.is_absolute() &&
            options.root.lexically_normal() == options.root && options.out_root.is_absolute() &&
            options.out_root.lexically_normal() == options.out_root && destination.out_root.is_absolute() &&
            destination.out_root.lexically_normal() == destination.out_root &&
            options.profile.id == destination.profile.id && profile_hash == profile_config_hash(destination.profile) &&
            options.profile.config_source == destination.profile.config_source,
            "source-discovery-binding-mismatch");
    deadline = SteadyClock::now() + std::chrono::seconds(30);
    transfer_ms = transfer_verify_ms = transfer_snapshot_ms = 0;
    const auto transfer_started = SteadyClock::now();
    bool transfer_attempted = false;
    try {
        const auto candidate_freshness = validate_current();
        require(destination.generation_admission->assess(destination).snapshot_id == snapshot_id,
                "source-discovery-snapshot-changed");
        auto verification_started = SteadyClock::now();
        options.generation_admission->verify_content_together(options,
            *destination.generation_admission, destination);
        transfer_verify_ms += elapsed_ms_since(verification_started);
        const auto snapshot_started = SteadyClock::now();
        RetrievalSession published(destination, RetrievalSessionOpenMode::verified_snapshot);
        const auto& published_freshness = published.freshness();
        require(candidate_freshness.snapshot_id == snapshot_id && published_freshness.snapshot_id == snapshot_id,
                "source-discovery-snapshot-changed");
        const auto published_sources = source_bindings(published);
        transfer_snapshot_ms = elapsed_ms_since(snapshot_started);
        require(published_sources == sources, "source-discovery-binding-mismatch");
        // The joint byte pass above already fenced both immutable admissions.
        // The verified snapshot and exact source-map comparison bind the cache.
        // Repeating assess here cannot strengthen the final joint pass below.
        const auto binding = source_cache_binding(destination, profile_hash, snapshot_id);
        SourceCache cache(cache_directory, fnv1a64(source_cache_identity(destination).dump()).substr(8));
        const auto persist_started = SteadyClock::now();
        transfer_attempted = true;
        persist(cache, binding);
        persist_ms += elapsed_ms_since(persist_started);
        persisted_cache = true;
        cache_status = "persisted";
        // Retain profile-file and complete before/after byte/dependency fences.
        // The admissions cannot change snapshot identity during this transfer.
        check_profile();
        verification_started = SteadyClock::now();
        options.generation_admission->verify_content_together(options,
            *destination.generation_admission, destination);
        transfer_verify_ms += elapsed_ms_since(verification_started);
        check_profile();
        transfer_ms = elapsed_ms_since(transfer_started);
        check_time();
    } catch (...) {
        if (transfer_attempted) invalidated = true;
        throw;
    }
}

Json SourceDiscoveryIndex::Impl::preparation_metrics() const {
    std::lock_guard metrics_lock(mutex);
    return {{"files", files}, {"source_bytes", total_bytes}, {"chunks", chunks},
            {"cache_status", persisted_cache && cache_status == "disabled" ? "persisted" : cache_status},
            {"index_builds", cache_status == "restored" ? 0 : 1},
            {"read_scan_ms", read_scan_ms}, {"chunking_ms", chunking_ms},
            {"sql_index_ms", sql_index_ms}, {"persist_ms", persist_ms},
            {"preparation_ms", preparation_ms}, {"transfer_ms", transfer_ms},
            {"transfer_verify_ms", transfer_verify_ms}, {"transfer_snapshot_ms", transfer_snapshot_ms}};
}

Json SourceDiscoveryIndex::Impl::query(const RuntimeOptions& binding, const std::vector<std::string>& queries,
                                       bool materialize, bool at_snapshot) {
    validate_queries(queries);
    require(!materialize || queries.size() == 1, "source-context-query-limit");
    require(binding.root == options.root && binding.out_root == options.out_root &&
            binding.profile.id == options.profile.id && profile_config_hash(binding.profile) == profile_hash &&
            binding.profile.config_source == options.profile.config_source &&
            (at_snapshot ? options.generated_snapshot && binding.generated_snapshot == options.generated_snapshot
                         : !binding.generated_snapshot || binding.generated_snapshot == options.generated_snapshot),
            "source-discovery-binding-mismatch");
    std::unique_lock request_lock(mutex, std::try_to_lock);
    require(request_lock.owns_lock(), "source-discovery-busy");
    require(!invalidated, "source-discovery-invalidated");
    const auto started = SteadyClock::now();
    deadline = started + std::chrono::seconds(30);
    std::unique_ptr<shin::generated_io::IndexLock> publication_lock;
    try {
        publication_lock = lock_index_for_read(options.out_root);
    } catch (...) {
        invalidated = true;
        throw;
    }
    if (at_snapshot) check_profile();
    else (void)validate_current();
    const auto admission_ms = elapsed_ms_since(started);
    sqlite3* raw = database.get();
    auto record = prepare(raw, "SELECT payload FROM cache_meta WHERE id=?");
    Json results = Json::array();
    for (const auto& query : queries) {
        check_time();
        const auto query_started = SteadyClock::now();
        execute(raw, "DELETE FROM query_text;");
        auto input = prepare(raw, "INSERT INTO query_text(body) VALUES (?)");
        bind_text(input, 1, query);
        require(sqlite3_step(input.get()) == SQLITE_DONE, "source-discovery-sql");
        auto vocabulary = prepare(raw, "SELECT term FROM query_vocab ORDER BY term");
        std::vector<std::string> tokens;
        int rc;
        while ((rc = sqlite3_step(vocabulary.get())) == SQLITE_ROW) {
            require(tokens.size() < 64, "source-discovery-token-limit");
            tokens.push_back(column_text(vocabulary.get(), 0));
        }
        require(rc == SQLITE_DONE, "source-discovery-sql");
        const auto terms = source_query_terms(tokens);
        const auto distinct_paths_first = source_query_distinct_paths_first(tokens);
        require(terms.size() <= 256, "source-discovery-token-limit");
        // Several spellings may stem to one word (cancelled / cancelling).
        // FTS counts repeated OR phrases repeatedly; keep one representative
        // per stem so inflections cannot multiply that concept's BM25 weight.
        execute(raw, "DELETE FROM query_stems;");
        auto stem_input = prepare(raw, "INSERT INTO query_stems(rowid,body) VALUES (?,?)");
        for (std::size_t i = 0; i < terms.size(); ++i) {
            require(sqlite3_bind_int64(stem_input.get(), 1, static_cast<sqlite3_int64>(i) + 1) == SQLITE_OK,
                    "source-discovery-sql");
            bind_text(stem_input, 2, terms[i]);
            require(sqlite3_step(stem_input.get()) == SQLITE_DONE && sqlite3_reset(stem_input.get()) == SQLITE_OK,
                    "source-discovery-sql");
        }
        auto stems = prepare(raw, "SELECT min(doc) FROM stem_vocab GROUP BY term ORDER BY term");
        std::string expression;
        while ((rc = sqlite3_step(stems.get())) == SQLITE_ROW) {
            const auto index = sqlite3_column_int64(stems.get(), 0);
            require(index > 0 && static_cast<std::size_t>(index) <= terms.size(), "source-discovery-sql");
            const auto& term = terms[static_cast<std::size_t>(index) - 1];
            if (!expression.empty()) expression += " OR ";
            expression += '"';
            for (const auto c : term) {
                expression += c;
                if (c == '"') expression += c;
            }
            expression += '"';
        }
        require(rc == SQLITE_DONE, "source-discovery-sql");
        Json candidates = Json::array();
        Json deferred = Json::array();
        std::map<std::string, int> per_path;
        const auto append_candidate = [&](Json candidate) {
            if (at_snapshot || cache_status == "restored") {
                const auto content = read_bound_source(options.root, candidate.at("path"),
                                                       candidate.at("source_content_hash"), kFileBytes);
                const auto begin = candidate.at("byte_start").get<std::size_t>();
                const auto end = candidate.at("byte_end").get<std::size_t>();
                const auto offsets = source_line_offsets(content);
                require(end <= content.size() && fnv1a64(content.substr(begin, end - begin)) == candidate.at("span_hash").get<std::string>() &&
                        static_cast<std::size_t>(std::upper_bound(offsets.begin(), offsets.end(), begin) - offsets.begin()) == candidate.at("start_line").get<std::size_t>() &&
                        static_cast<std::size_t>(std::upper_bound(offsets.begin(), offsets.end(), end - 1) - offsets.begin()) == candidate.at("end_line").get<std::size_t>() &&
                        !source_range_secret(content).found, "source-cache-source-mismatch");
            }
            candidate["rank"] = candidates.size() + 1;
            candidates.push_back(std::move(candidate));
        };
        if (!expression.empty()) {
            auto search = prepare(raw, "SELECT rowid,bm25(chunks) AS score FROM chunks WHERE chunks MATCH ? ORDER BY score,rowid");
            bind_text(search, 1, expression);
            while ((rc = sqlite3_step(search.get())) == SQLITE_ROW) {
                const auto id = sqlite3_column_int64(search.get(), 0);
                require(id > 0 && static_cast<std::size_t>(id) <= chunks, "source-discovery-row-binding");
                require(sqlite3_bind_int64(record.get(), 1, id) == SQLITE_OK && sqlite3_step(record.get()) == SQLITE_ROW,
                        "source-discovery-row-binding");
                auto candidate = Json::parse(column_text(record.get(), 0));
                require(sqlite3_reset(record.get()) == SQLITE_OK, "source-discovery-sql");
                const auto occurrence = ++per_path[candidate.at("path").get<std::string>()];
                if (occurrence > 2) continue;
                candidate["bm25"] = sqlite3_column_double(search.get(), 1);
                if (distinct_paths_first && occurrence == 2) deferred.push_back(std::move(candidate));
                else append_candidate(std::move(candidate));
                if (candidates.size() == 8) break;
            }
            require(rc == SQLITE_ROW || rc == SQLITE_DONE, "source-discovery-sql");
            for (auto& candidate : deferred) {
                if (candidates.size() == 8) break;
                append_candidate(std::move(candidate));
            }
        }
        results.push_back({{"query", query}, {"candidates", candidates}, {"query_ms", elapsed_ms_since(query_started)},
                           {"distinct_paths_first", distinct_paths_first},
                           {"answer_complete", false}, {"source_inspection_required", true}});
    }
    const auto collection_started = SteadyClock::now();
    Json context;
    if (materialize) context = collect_source_context(options, snapshot_id, results[0].at("candidates"), sources);
    const auto collection_ms = elapsed_ms_since(collection_started);
    // Explicit snapshot calls verify returned sources above; the owner supplies
    // final source/authority checks. Regular calls still recheck the corpus.
    const auto revalidation_started = SteadyClock::now();
    IndexFreshness freshness;
    if (at_snapshot) {
        check_profile();
        freshness = RetrievalSession(options.generated_snapshot, false).freshness();
    } else freshness = validate_current();
    check_time();
    Json result = {{"schema_version", "shin.source-candidates.v1"}, {"ok", true},
        {"profile", options.profile.id}, {"root", options.root.generic_string()},
        {"snapshot_id", freshness.snapshot_id}, {"index_freshness", index_freshness_json(freshness)},
        {"policy", "source-software-terms-en-ru-porter-bm25-diversity-v4"},
        {"admission", at_snapshot ? "immutable-publication-and-selected-source-hashes" :
            "verified-publication-and-uncached-source-hashes-not-output-regeneration"},
        {"sqlite_version", sqlite3_libversion()}, {"model_runs", 0}, {"default_brief_changed", false},
        {"files", files}, {"chunks", chunks}, {"source_bytes", total_bytes},
        {"indexed_text_bytes", indexed_text_bytes},
        {"redacted_files", redacted_files}, {"fallback_chunks", fallback_chunks},
        {"limits", {{"max_source_bytes", kSourceBytes}, {"max_file_bytes", kFileBytes},
                    {"max_chunks", kRows}, {"records_per_query", 8}, {"records_per_file", 2}}},
        {"preparation_ms", successful_queries == 0 ? preparation_ms : 0.0},
        {"index_preparation_ms", preparation_ms}, {"admission_ms", admission_ms},
        {"batch_ms", elapsed_ms_since(started)},
        {"total_ms", elapsed_ms_since(started) + (successful_queries == 0 ? preparation_ms : 0.0)},
        {"reuse", {{"index_builds", cache_status == "restored" ? 0 : 1}, {"query_sequence", successful_queries + 1},
                    {"reused", successful_queries > 0}, {"persistent", cache_status != "disabled"}}},
        {"cache", {{"status", cache_status}, {"derived", true}, {"maintenance", "immutable-replacement-no-wal"}}},
        {"revalidation_ms", elapsed_ms_since(revalidation_started)},
        {"process_peak_rss_bytes", process_peak_rss_bytes()}, {"sqlite_process_memory_bytes", sqlite3_memory_used()},
        {"results", std::move(results)}};
    if (materialize) {
        result["schema_version"] = "shin.source-context.v1";
        result["context"] = std::move(context);
        result["collection_ms"] = collection_ms;
    }
    require(result.dump().size() <= (materialize ? 128 : 512) * 1024, "source-discovery-output-limit");
    ++successful_queries;
    return result;
}

SourceDiscoveryIndex::SourceDiscoveryIndex(RuntimeOptions options, const fs::path& cache_directory)
    : impl_(std::make_unique<Impl>(std::move(options), cache_directory)) {}
SourceDiscoveryIndex::~SourceDiscoveryIndex() = default;
Json SourceDiscoveryIndex::query(const RuntimeOptions& binding, const std::vector<std::string>& queries) {
    return impl_->query(binding, queries);
}
Json SourceDiscoveryIndex::context(const RuntimeOptions& binding, const std::string& query) {
    return impl_->query(binding, {query}, true);
}
void SourceDiscoveryIndex::persist_published_cache(const RuntimeOptions& destination,
                                                   const fs::path& cache_directory) {
    impl_->persist_published_cache(destination, cache_directory);
}
Json SourceDiscoveryIndex::preparation_metrics() const {
    return impl_->preparation_metrics();
}
Json SourceDiscoveryIndex::query_at_snapshot(const RuntimeOptions& binding, const std::vector<std::string>& queries) {
    return impl_->query(binding, queries, false, true);
}
Json SourceDiscoveryIndex::context_at_snapshot(const RuntimeOptions& binding, const std::string& query) {
    return impl_->query(binding, {query}, true, true);
}

Json discover_source_candidates(const RuntimeOptions& options, const std::vector<std::string>& queries) {
    validate_queries(queries);
    SourceDiscoveryIndex index(options);
    return index.query(options, queries);
}
}  // namespace shin::context
