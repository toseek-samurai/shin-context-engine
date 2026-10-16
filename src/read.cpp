#include "context_internal.h"
#include "execution_io.h"
#include "generated_io.h"
#include "generated_row_decoder.h"
#include "generated_snapshot.h"
#include "session_contract.h"
#include "source_read.h"

#include <tuple>

namespace shin::context {
namespace {

constexpr std::size_t kReadEnvelopeBytes = 128 * 1024;
constexpr std::size_t kReadEvidenceBytes = 32 * 1024;
constexpr std::size_t kReadRecords = 8;
constexpr std::size_t kReadFiles = 4;
constexpr std::size_t kReadFileBytes = 1024 * 1024;
constexpr std::size_t kReadSourceBytes = 4 * kReadFileBytes;
constexpr std::size_t kReadFactStoreBytes = 64 * 1024 * 1024;
using Range = std::tuple<std::string, int, int>;

struct ReadRequest {
    std::string snapshot;
    std::vector<std::string> ids;
    bool whole_files = false;
};

ReadRequest parse_read_request(std::string_view input) {
    if (input.size() > kReadEnvelopeBytes)
        throw_usage_error("source-read-input-too-large", "Source read request exceeds 128 KiB.");
    // Reuse the bounded duplicate-key/depth/secret-safe request parser. Never
    // forward a JSON parser diagnostic that could contain private input bytes.
    Json value;
    try { value = parse_session_document(input); }
    catch (const CliError&) {
        throw_usage_error("source-read-invalid-json", "Source read request is not valid safe JSON.");
    }
    ReadRequest request;
    const auto schema = json_string_value(value, "schema_version");
    request.whole_files = schema == "shin.source-read-request.v2";
    const auto selector = request.whole_files ? "file_paths" : "record_ids";
    if (!value.is_object() || value.size() != 3 ||
        (!request.whole_files && schema != "shin.source-read-request.v1") ||
        !value.contains(selector) || !value.at(selector).is_array())
        throw_usage_error("source-read-invalid-request", "Use v1 record_ids or v2 file_paths with snapshot_id only.");
    request.snapshot = json_string_value(value, "snapshot_id");
    if (request.snapshot.size() != 24 || !starts_with(request.snapshot, "fnv1a64:") ||
        !std::all_of(request.snapshot.begin() + 8, request.snapshot.end(), [](char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        }))
        throw_usage_error("source-read-invalid-snapshot", "Source read requires an exact snapshot identity.");
    const auto& ids = value.at(selector);
    if (ids.empty() || ids.size() > (request.whole_files ? kReadFiles : kReadRecords))
        throw_usage_error(request.whole_files ? "source-read-file-budget" : "source-read-record-budget",
                          "Select 1-8 published records or 1-4 published files.");
    for (const Json& id : ids) {
        if (!id.is_string()) throw_usage_error("source-read-invalid-id", "Record identifiers must be strings.");
        const auto text = id.get<std::string>();
        if (text.empty() || text.size() > (request.whole_files ? 4096 : 1024) || std::any_of(text.begin(), text.end(), [](unsigned char c) {
                return c < 32 || c == 127;
            }))
            throw_usage_error("source-read-invalid-id", "Source selectors require bounded non-control text.");
        if (request.whole_files && (!profile_relative_path_valid(text) ||
            fs::path(text).lexically_normal().generic_string() != text))
            throw_usage_error("source-read-invalid-path", "File selectors must be canonical repository-relative paths.");
        request.ids.push_back(text);
    }
    return request;
}

std::map<std::string, FactRow> selected_rows(const RuntimeOptions& options, const Json& manifest,
                                          const ReadRequest& request) {
    const auto key = (kGeneratedDir / "facts.jsonl").generic_string();
    const auto& entries = manifest.at("generated_file_integrity").at("files");
    const auto entry = std::find_if(entries.begin(), entries.end(), [&](const Json& row) {
        return json_string_value(row, "path") == key;
    });
    if (entry == entries.end()) throw_index_error("index-corrupt", "Missing published fact store binding.", false);
    const auto bytes = entry->at("bytes").get<std::uintmax_t>();
    if (bytes > kReadFactStoreBytes)
        throw CliError("source-read-fact-store-budget", "resource", "Published fact store exceeds the 64 MiB read bound.",
                       1, false, "Select a bounded project profile before an explicit rebuild.");
    const auto hash = json_string_value(*entry, "content_hash");
    const std::string content = options.generated_snapshot
        ? std::string(options.generated_snapshot->file(options.out_root / key))
        : read_bound_source(options.out_root, key, hash, kReadFactStoreBytes);
    if (content.size() != bytes || fnv1a64(content) != hash)
        throw_index_error("source-read-fact-store-changed", "Published fact store changed during source selection.");
    const std::set<std::string> wanted(request.ids.begin(), request.ids.end());
    std::map<std::string, FactRow> selected;
    std::istringstream stream(content);
    std::string line;
    while (std::getline(stream, line)) {
        auto row = decode_generated_fact_row(line);
        if (wanted.contains(row.id) && !selected.emplace(row.id, row).second)
            throw_index_error("source-read-duplicate-id", "Published record identity is ambiguous.", false);
    }
    if (selected.size() != wanted.size())
        throw_usage_error("source-read-unknown-record", "At least one selected record is absent from this snapshot.");
    return selected;
}

IndexFreshness require_read_current(const RuntimeOptions& options, const std::string& snapshot) {
    auto current = assess_index_freshness(options, true);
    require_current_index_or_throw(current);
    if (!current.fully_verified || current.snapshot_id != snapshot)
        throw_index_error("source-read-snapshot-mismatch", "Source read requires the selected current publication.");
    return current;
}

Json record_metadata(const FactRow& row, const std::vector<std::size_t>& positions) {
    return {{"record_id", row.id}, {"request_indices", positions}, {"record_type", row.record_type},
        {"path", row.path}, {"start_line", row.start_line}, {"end_line", row.end_line},
        {"source", row.source}, {"range_origin", row.range_origin}, {"span_hash", row.span_hash},
        {"status", row.status}, {"authority_level", row.authority_level},
        {"evidence_kind", row.evidence_kind}, {"verification_level", row.verification_level}};
}

struct MaterializedRange {
    Json body;
    bool sensitive = false;
};

struct SourceSelection {
    std::string path;
    int start = 1;
    int end = 1;
    std::string span_hash;
    Json metadata;
};

FileMetadata source_metadata(const fs::path& path) {
    try { return file_metadata(path); }
    catch (const std::runtime_error&) {
        throw CliError("source-read-unavailable", "filesystem",
                       "Selected source metadata is unavailable or its path is unsafe.", 1, true,
                       "Retry after source changes settle; unsafe paths remain refused.");
    }
}

}  // namespace

Json RetrievalSession::read(std::string_view input) const {
    return read_impl(input, false);
}

Json RetrievalSession::read_selected(std::string_view input) const {
    if (!options_.generated_snapshot)
        throw_usage_error("invalid-snapshot-session", "Selected source read requires an immutable snapshot owner.");
    return read_impl(input, true);
}

Json RetrievalSession::read_impl(std::string_view input, bool selected_only) const {
    const ReadRequest request = parse_read_request(input);
    if (options_.force_source_scan)
        throw_usage_error("unsupported-option", "Source read requires a published snapshot.");
    if (request.snapshot != freshness_.snapshot_id)
        throw_index_error("source-read-snapshot-mismatch", "Requested snapshot differs from the admitted session.", false);
    auto lock = lock_ ? nullptr : lock_index_for_read(options_.out_root);
    const auto current = selected_only ? RetrievalSession(options_.generated_snapshot, false).freshness()
                                       : require_read_current(options_, request.snapshot);
    std::map<std::string, Json> sources;
    for (const Json& source : manifest().at("source_fact_sources")) {
        if (json_string_value(source, "status") == "scanned" &&
            !sources.emplace(json_string_value(source, "path"), source).second)
            throw_index_error("source-read-invalid-binding", "Scanned source binding is ambiguous.", false);
    }
    std::set<std::string> files;
    std::map<std::string, std::string> omissions;
    std::map<std::string, SourceSelection> rows;
    if (request.whole_files) {
        for (const auto& path : request.ids) {
            if (!sources.contains(path))
                throw_usage_error("source-read-unknown-file", "At least one selected file has no scanned source binding.");
            rows.emplace(path, SourceSelection{path, 1, 1, "",
                {{"path", path}, {"selection_scope", "whole-published-source-file"},
                 {"verification_level", "source-observed"}}});
            files.insert(path);
        }
    } else {
        for (const auto& [id, row] : selected_rows(options_, manifest(), request)) {
            rows.emplace(id, SourceSelection{row.path, row.start_line, row.end_line, row.span_hash,
                                            record_metadata(row, {})});
            if (!starts_with(row.source, "source-fact:") || row.span_hash.empty()) {
                omissions[id] = "not-source-range";
                continue;
            }
            if (!sources.contains(row.path))
                throw_index_error("source-read-invalid-binding", "Selected fact has no scanned source binding.", false);
            files.insert(row.path);
        }
    }
    if (files.size() > kReadFiles)
        throw_usage_error("source-read-file-budget", "Source read selects at most four files.");
    std::map<Range, MaterializedRange> ranges;
    std::map<std::string, FileMetadata> observed;
    std::size_t source_bytes = 0;
    for (const auto& path : files) {
        const auto& binding = sources.at(path);
        if (binding.at("bytes").get<std::uintmax_t>() > kReadFileBytes) {
            for (const auto& [id, row] : rows) if (row.path == path) omissions[id] = "source-file-too-large";
            continue;
        }
        const auto before = source_metadata(options_.root / path);
        const auto hash = json_string_value(binding, "content_hash");
        const auto content = read_bound_source(options_.root, path, hash, kReadFileBytes);
        if (source_metadata(options_.root / path) != before) throw_file_changed_during_verification(options_.root / path);
        observed.emplace(path, before);
        if (content.size() > kReadSourceBytes - source_bytes)
            throw CliError("source-read-byte-budget", "resource", "Source materialization exceeds four MiB.",
                           1, false, "Select fewer source files.");
        source_bytes += content.size();
        const auto offsets = source_line_offsets(content);
        for (auto& [id, row] : rows) {
            if (row.path != path || omissions.contains(id)) continue;
            if (request.whole_files) {
                // A terminal newline is not an additional physical source line.
                row.end = static_cast<int>(offsets.size() - (!content.empty() && content.back() == '\n' ? 1 : 0));
                row.span_hash = hash;
                row.metadata.update({{"start_line", 1}, {"end_line", row.end}, {"source_content_hash", hash}});
            }
            const Range key{path, row.start, row.end};
            const auto raw = source_physical_range(content, offsets, row.start, row.end);
            if (fnv1a64(raw) != row.span_hash)
                throw_index_error("source-read-span-mismatch", "Selected span does not match its source bytes.", false);
            if (ranges.contains(key)) continue;
            const auto hit = source_range_secret(raw);
            Json body = {{"path", path}, {"start_line", row.start}, {"end_line", row.end},
                {"span_hash", row.span_hash}, {"source_content_hash", hash}, {"source_range_bytes", raw.size()},
                {"state", hit.found ? "redacted" : "complete"},
                {"reason", hit.found ? Json(hit.kind) : Json(nullptr)}, {"text", hit.found ? Json(nullptr) : Json(raw)}};
            ranges.emplace(key, MaterializedRange{std::move(body), hit.found});
        }
    }
    // Scan every selected range first, even if its body will not fit the output.
    // A sensitive enclosing/overlapping range also suppresses selected neighbors.
    for (auto& [key, materialized] : ranges) {
        for (const auto& [other, source] : ranges) {
            if (source.sensitive && std::get<0>(key) == std::get<0>(other) &&
                std::get<1>(other) <= std::get<2>(key) && std::get<2>(other) >= std::get<1>(key)) {
                materialized.body["state"] = "redacted";
                materialized.body["reason"] = "sensitive-overlapping-source-range";
                materialized.body["text"] = nullptr;
                break;
            }
        }
    }
    const auto selection_key = request.whole_files ? "files" : "records";
    const auto identity_key = request.whole_files ? "path" : "record_id";
    Json evidence = {{selection_key, Json::array()}, {"bodies", Json::array()}, {"omissions", Json::array()}};
    std::map<std::string, std::vector<std::size_t>> positions;
    for (std::size_t i = 0; i < request.ids.size(); ++i) positions[request.ids[i]].push_back(i);
    std::set<std::string> emitted;
    std::map<Range, std::size_t> body_indices;
    bool complete = true;
    for (const auto& id : request.ids) {
        if (!emitted.insert(id).second) continue;
        const auto& row = rows.at(id);
        const Range key{row.path, row.start, row.end};
        std::string reason = omissions.contains(id) ? omissions.at(id) : "";
        if (reason.empty()) {
            Json candidate = evidence;
            Json record = row.metadata;
            record["request_indices"] = positions.at(id);
            const bool added_body = !body_indices.contains(key);
            const auto index = added_body ? candidate["bodies"].size() : body_indices.at(key);
            record["body_index"] = index;
            candidate[selection_key].push_back(std::move(record));
            if (added_body) candidate["bodies"].push_back(ranges.at(key).body);
            // Reserve compact omission descriptors for every later requested ID.
            Json bounded = candidate;
            for (const auto& [pending, indices] : positions) {
                if (!emitted.contains(pending)) bounded["omissions"].push_back({
                    {identity_key, pending}, {"request_indices", indices}, {"reason", "serialized-evidence-budget"}});
            }
            if (bounded.dump().size() <= kReadEvidenceBytes) {
                evidence = std::move(candidate);
                if (added_body) body_indices.emplace(key, index);
                complete = complete && ranges.at(key).body.at("state") == "complete";
                continue;
            }
            reason = "serialized-evidence-budget";
        }
        complete = false;
        evidence["omissions"].push_back({{identity_key, id}, {"request_indices", positions.at(id)}, {"reason", reason}});
    }
    Json result = make_result_document(request.whole_files ? "shin.source-read.v2" : "shin.source-read.v1", "read", true);
    result.update({{"profile", options_.profile.id}, {"snapshot_id", request.snapshot},
        {"generator_version", kGeneratorVersion}, {"index_freshness", index_freshness_json(current)},
        {"complete", complete}, {request.whole_files ? "requested_files" : "requested_records", request.ids.size()},
        {request.whole_files ? "unique_files" : "unique_records", rows.size()},
        {"materialization_source_bytes", source_bytes}, {"evidence_bytes", evidence.dump().size()},
        {"limits", {{"max_request_bytes", kReadEnvelopeBytes}, {"max_output_bytes", kReadEnvelopeBytes},
                    {"max_records", kReadRecords}, {"max_files", kReadFiles}, {"max_source_file_bytes", kReadFileBytes},
                    {"max_materialization_source_bytes", kReadSourceBytes}, {"max_evidence_bytes", kReadEvidenceBytes},
                    {"max_fact_store_bytes", kReadFactStoreBytes}}},
        {"interpretation", {{"scope", request.whole_files ? "selected-published-source-files" : "selected-published-physical-ranges"}, {"read_only", true},
            {"complete_means", request.whole_files ? "all-selected-files-delivered-unredacted" : "all-selected-ranges-delivered-unredacted"}, {"answer_completeness", "not-asserted"},
            {"source_byte_scope", selected_only ? "selected-source-files-only" :
                "materialization-only-excludes-existing-current-admission-verification"},
            {"hash_authentication", false}, {"execution_proven", false}}}, {"evidence", std::move(evidence)}});
    if (result.at("evidence_bytes").get<std::size_t>() > kReadEvidenceBytes || result.dump().size() + 1 > kReadEnvelopeBytes)
        throw CliError("source-read-output-budget", "resource", "Source read output exceeds its serialized byte budget.",
                       1, false, "Select fewer records.");
    if (!selected_only) (void)require_read_current(options_, request.snapshot);
    for (const auto& [path, before] : observed) {
        if (profile_path_contains_symlink(options_.root, path) || source_metadata(options_.root / path) != before)
            throw_file_changed_during_verification(options_.root / path);
    }
    return result;
}

int run_read(const RuntimeOptions& options, const std::vector<std::string>& args) {
    if (args.size() != 2 || args.front() != "--input")
        throw_usage_error("invalid-read-arguments", "Use read --input FILE|-; current-source verification is mandatory.");
    std::string input;
    if (args[1] == "-") {
        std::array<char, 8192> buffer{};
        while (std::cin) {
            std::cin.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            const auto bytes = static_cast<std::size_t>(std::cin.gcount());
            if (bytes > kReadEnvelopeBytes - input.size())
                throw_usage_error("source-read-input-too-large", "Source read request exceeds 128 KiB.");
            input.append(buffer.data(), bytes);
        }
        if (std::cin.bad()) throw_usage_error("source-read-input-unavailable", "Cannot read the source request.");
    } else {
        input = read_execution_report_file(args[1], kReadEnvelopeBytes);
    }
    (void)parse_read_request(input);
    if (options.force_source_scan) throw_usage_error("unsupported-option", "Source read requires a published snapshot.");
    RetrievalSession session(options, RetrievalSessionOpenMode::verified_snapshot);
    const auto result = session.read(input);
    std::cout << result.dump() << '\n';
    return 0;
}

}  // namespace shin::context
