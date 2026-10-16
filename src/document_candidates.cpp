#include "context_internal.h"
#include "generated_snapshot.h"

namespace shin::context {
namespace {

constexpr std::string_view kOwnerIndexSchema = "shin.document-row-index.v1";
constexpr std::size_t kMaxOwnerIndexBytes = 16 * 1024 * 1024;
constexpr std::size_t kMaxOwnerDocuments = 65536;
constexpr std::array<std::string_view, 5> kStores{"sections", "facts", "decisions", "contracts", "runbooks"};
constexpr std::array kMembers{&CandidateRows::sections, &CandidateRows::facts, &CandidateRows::decisions,
                              &CandidateRows::contracts, &CandidateRows::runbooks};

struct OwnerRange {
    int begin;
    int end;
    std::size_t owner;
};

struct OwnerIndex {
    std::size_t documents = 0;
    std::vector<std::string> paths;
    std::array<std::vector<OwnerRange>, kStores.size()> stores;
};

OwnerIndex read_owner_index(const RuntimeOptions& options) {
    const auto file = options.out_root / kGeneratedDir / "document-rows.json";
    GeneratedInput input(options, file);
    if (!input) throw_index_error("index-corrupt", "Document row index is missing.");
    std::string bytes;
    std::array<char, 8192> buffer{};
    while (input.read(buffer.data(), buffer.size()) || input.gcount() > 0) {
        const auto count = static_cast<std::size_t>(input.gcount());
        if (bytes.size() + count > kMaxOwnerIndexBytes) {
            throw_index_error("index-corrupt", "Document row index exceeds its byte bound.");
        }
        bytes.append(buffer.data(), count);
    }
    if (input.bad()) throw_index_error("index-corrupt", "Cannot read document row index.");
    std::size_t owner_values = 0;
    std::size_t range_values = 0;
    // Reject oversized or deeply nested metadata before constructing its full DOM.
    const auto guard = [&](int depth, Json::parse_event_t event, Json& value) {
        using Event = Json::parse_event_t;
        if (depth > 4) throw_index_error("index-corrupt", "Document row index is too deeply nested.");
        if (depth == 2 && (event == Event::object_start || event == Event::array_start || event == Event::value) &&
            ++owner_values > kMaxOwnerDocuments + 16) {
            throw_index_error("index-corrupt", "Document row index has too many owners.");
        }
        if (event == Event::key) {
            const auto& key = value.get_ref<const std::string&>();
            const bool store = std::find(kStores.begin(), kStores.end(), key) != kStores.end();
            const bool allowed = (depth == 1 && (key == "schema_version" || key == "owners" || key == "totals")) ||
                                 (depth == 2 && store) || (depth == 3 && (store || key == "path"));
            if (!allowed) throw_index_error("index-corrupt", "Document row index contains an unknown field.");
        }
        if (event == Event::array_start && depth == 3) range_values = 0;
        if (event == Event::value && depth == 4 && ++range_values > 2) {
            throw_index_error("index-corrupt", "Document row range contains too many values.");
        }
        if (event == Event::value && depth == 3 && value.is_string() &&
            value.get_ref<const std::string&>().size() > 4096) {
            throw_index_error("index-corrupt", "Document row path exceeds its byte bound.");
        }
        return true;
    };
    Json value;
    try {
        value = Json::parse(bytes, guard);
    } catch (const Json::exception&) {
        throw_index_error("index-corrupt", "Document row index is not valid JSON.");
    }
    if (!value.is_object() || !value.contains("schema_version") || !value["schema_version"].is_string() ||
        value["schema_version"].get_ref<const std::string&>() != kOwnerIndexSchema ||
        !value.contains("owners") || !value["owners"].is_array() ||
        value["owners"].size() > kMaxOwnerDocuments ||
        !value.contains("totals") || !value["totals"].is_object()) {
        throw_index_error("index-corrupt", "Invalid document row index envelope.");
    }
    OwnerIndex result;
    std::string previous_path;
    std::array<int, kStores.size()> next{};
    for (const auto& owner : value["owners"]) {
        if (!owner.is_object() || !owner.contains("path") || !owner["path"].is_string()) {
            throw_index_error("index-corrupt", "Document row owner has no path.");
        }
        const auto path = owner.value("path", "");
        if (!profile_relative_path_valid(path) || (!previous_path.empty() && path <= previous_path)) {
            throw_index_error("index-corrupt", "Document row owners are not canonical and ordered.");
        }
        previous_path = path;
        result.paths.push_back(path);
        for (std::size_t store = 0; store < kStores.size(); ++store) {
            const auto range = owner.find(kStores[store]);
            if (range == owner.end()) continue;
            if (!range->is_array() || range->size() != 2 ||
                !(*range)[0].is_number_integer() || !(*range)[1].is_number_integer()) {
                throw_index_error("index-corrupt", "Invalid document row range.");
            }
            const auto begin = (*range)[0].get<std::int64_t>();
            const auto end = (*range)[1].get<std::int64_t>();
            if (begin != next[store] || end <= begin || end > std::numeric_limits<int>::max()) {
                throw_index_error("index-corrupt", "Document row ranges are not contiguous.");
            }
            result.stores[store].push_back({static_cast<int>(begin), static_cast<int>(end), result.documents});
            next[store] = static_cast<int>(end);
        }
        ++result.documents;
    }
    for (std::size_t store = 0; store < kStores.size(); ++store) {
        const auto count = value["totals"].find(kStores[store]);
        if (count == value["totals"].end() || !count->is_number_integer() ||
            count->get<std::int64_t>() != next[store]) {
            throw_index_error("index-corrupt", "Document row totals do not match their ranges.");
        }
    }
    return result;
}

std::size_t row_owner(const std::vector<OwnerRange>& ranges, int row) {
    const auto end = std::upper_bound(ranges.begin(), ranges.end(), row,
                                      [](int id, const OwnerRange& range) { return id < range.begin; });
    if (end == ranges.begin() || row >= std::prev(end)->end) {
        throw_index_error("index-corrupt", "Term posting row has no document owner.");
    }
    return std::prev(end)->owner;
}

} // namespace

std::set<std::string> whole_document_paths(const RuntimeOptions& options, const Json* manifest) {
    Json loaded;
    if (!manifest && options.generated_snapshot) manifest = &options.generated_snapshot->manifest();
    if (!manifest) {
        const auto path = options.out_root / kGeneratedDir / "manifest.json";
        loaded = parse_json_text(read_file(path), path.string());
        manifest = &loaded;
    }
    std::set<std::string> result;
    for (const auto& source : manifest->at("sources")) {
        const auto kind = source.value("kind", "");
        if (kind == "markdown" || kind == "json") result.insert(source.at("path").get<std::string>());
    }
    return result;
}

RetrievalUnitKey retrieval_unit_key(const SectionRow& row, const std::set<std::string>& documents) {
    RetrievalUnitKey key{row.path, documents.contains(row.path), row.evidence_kind,
                         row.verification_level, row.authority_level, row.status, {}};
    if (!key.whole_document) {
        key.record_key = Json::array({"section", row.document_id, row.row_type, row.anchor, row.line,
            row.start_line, row.end_line, row.chunk_index, row.level, row.heading, row.preview, row.topics,
            row.indexed_text, row.indexed_text_omitted_bytes}).dump();
    }
    return key;
}

RetrievalUnitKey retrieval_unit_key(const FactRow& row, const std::set<std::string>& documents) {
    RetrievalUnitKey key{row.path, documents.contains(row.path) && !starts_with(row.source, "source-fact:"),
                         row.evidence_kind, row.verification_level, row.authority_level, row.status, {}};
    if (!key.whole_document) {
        key.record_key = Json::array({"fact", row.id, row.record_type, row.line, row.start_line, row.end_line,
            row.title, row.summary, row.source, row.area, row.identifier_terms, row.symbol_name,
            row.span_hash, row.range_origin, row.topics}).dump();
    }
    return key;
}

std::string render_document_row_index(const GeneratedRows& rows) {
    std::map<std::string, Json> owners;
    Json totals = Json::object();
    const auto append = [&](std::string_view store, const auto& records) {
        totals[store] = records.size();
        for (std::size_t id = 0; id < records.size(); ++id) {
            auto& owner = owners[records[id].path];
            owner["path"] = records[id].path;
            auto& range = owner[store];
            if (range.is_null()) range = Json::array({id, id + 1});
            else range[1] = id + 1;
        }
    };
    append("sections", rows.sections);
    append("facts", rows.facts);
    append("decisions", rows.decisions);
    append("contracts", rows.contracts);
    append("runbooks", rows.runbooks);
    Json serialized = Json::array();
    for (auto& [path, owner] : owners) {
        (void)path;
        serialized.push_back(std::move(owner));
    }
    auto output = Json{{"schema_version", kOwnerIndexSchema}, {"totals", totals}, {"owners", serialized}}.dump() + "\n";
    if (owners.size() > kMaxOwnerDocuments || output.size() > kMaxOwnerIndexBytes) {
        throw_index_error("index-too-large", "Document row index exceeds its publication bounds.");
    }
    return output;
}

std::optional<std::vector<CandidateRows>> filter_document_coverage_candidates(
    const RuntimeOptions& options, const std::vector<CandidateRows>& groups, int required,
    const std::set<std::string>& documents, const CandidateRows* ordinary) {
    if (std::any_of(groups.begin(), groups.end(), [](const auto& group) {
            return !group.from_terms_index || !group.term_index_coverage_complete;
        })) return std::nullopt;
    const auto index = read_owner_index(options);
    std::set<std::size_t> admitted;
    if (ordinary != nullptr) {
        for (std::size_t store = 0; store < kStores.size(); ++store) {
            if (store == 1) continue;
            for (const int row : ordinary->*kMembers[store]) {
                admitted.insert(row_owner(index.stores[store], row));
            }
        }
    }
    std::vector<std::set<std::size_t>> covered(index.documents);
    for (std::size_t group = 0; group < groups.size(); ++group) {
        for (std::size_t store = 0; store < kStores.size(); ++store) {
            for (const int row : groups[group].*kMembers[store]) {
                const auto owner = row_owner(index.stores[store], row);
                if (store != 1 && documents.contains(index.paths[owner])) covered[owner].insert(group);
            }
        }
    }
    std::vector<CandidateRows> result(groups.size());
    for (std::size_t group = 0; group < groups.size(); ++group) {
        auto& selected = result[group];
        selected.term_index_coverage_complete = true;
        selected.from_terms_index = true;
        for (std::size_t store = 0; store < kStores.size(); ++store) {
            for (const int row : groups[group].*kMembers[store]) {
                const auto owner = row_owner(index.stores[store], row);
                const auto& document = covered[owner];
                if (store != 1 && !admitted.contains(owner) && documents.contains(index.paths[owner]) &&
                    document.size() >= static_cast<std::size_t>(required)) {
                    (selected.*kMembers[store]).insert(row);
                    ++selected.discovered_row_count;
                }
            }
        }
    }
    return result;
}

} // namespace shin::context
