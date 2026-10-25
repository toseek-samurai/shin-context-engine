#include "context_internal.h"
#include "profile_quality.h"
#include "row_batch_experiment.h"

#include <algorithm>
#include <array>
#include <ctime>
#include <cstdint>
#include <iostream>
#include <iterator>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <sys/resource.h>
#include <tuple>
#include <type_traits>

namespace {
using namespace shin::context;
using namespace shin::context::experiment;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

enum class Store : unsigned char { section, fact, decision, contract, runbook };
using RowKey = std::pair<Store, int>;

struct QueryStage {
    std::string label;
    QueryRequest request;
    int required = 0;
    CandidateRows candidates;
    std::set<RowKey> members;
    bool relaxed_stage = false;
};

struct RowEntry {
    RowKey key;
    bool is_fact = false;
    SectionRow section;
    FactRow fact;
};

struct BatchData {
    std::size_t first = 0;
    std::vector<std::size_t> query_indices;
    CandidateRows candidates;
    std::size_t estimated_payload = 0;
    std::size_t pairs = 0;
};

struct ScoreDigest {
    std::uint64_t hash = 1469598103934665603ULL;
    std::size_t count = 0;
    void add(const MatchScore& score) {
        auto bytes = [&](const void* ptr, std::size_t size) {
            const auto* data = static_cast<const unsigned char*>(ptr);
            for (std::size_t i = 0; i < size; ++i) { hash ^= data[i]; hash *= 1099511628211ULL; }
        };
        bytes(&score.total, sizeof(score.total));
        bytes(&score.matched_groups, sizeof(score.matched_groups));
        for (int index : score.matched_group_indices) bytes(&index, sizeof(index));
        for (const auto& [name, value] : score.components) {
            bytes(name.data(), name.size()); bytes(&value, sizeof(value));
        }
        for (const auto& name : score.matched_fields) bytes(name.data(), name.size());
        ++count;
    }
    std::string text() const { return std::to_string(hash); }
};

std::size_t string_payload(const std::string& value) { return value.capacity() + 1; }
std::size_t strings_payload(const std::vector<std::string>& values) {
    std::size_t size = values.capacity() * sizeof(std::string);
    for (const auto& value : values) size += string_payload(value);
    return size;
}
std::size_t row_payload(const SectionRow& r) {
    return sizeof(r) + string_payload(r.document_id) + string_payload(r.path) + string_payload(r.status) +
        string_payload(r.authority_level) + string_payload(r.heading) + string_payload(r.anchor) +
        string_payload(r.preview) + string_payload(r.indexed_text) + strings_payload(r.topics) +
        string_payload(r.row_type) + string_payload(r.evidence_kind) + string_payload(r.verification_level);
}
std::size_t row_payload(const FactRow& r) {
    return sizeof(r) + string_payload(r.id) + string_payload(r.record_type) + string_payload(r.path) +
        string_payload(r.status) + string_payload(r.area) + string_payload(r.title) +
        string_payload(r.summary) + string_payload(r.source) + string_payload(r.authority_level) +
        strings_payload(r.topics) + strings_payload(r.identifier_terms) + string_payload(r.evidence_kind) +
        string_payload(r.verification_level) + string_payload(r.symbol_name) + string_payload(r.span_hash) +
        string_payload(r.range_origin);
}
std::size_t fields_payload(const std::vector<FieldMatchInput>& fields) {
    std::size_t size = fields.capacity() * sizeof(FieldMatchInput);
    for (const auto& field : fields) {
        size += field.lexical_terms.capacity() * sizeof(std::string);
        for (const auto& term : field.lexical_terms) size += string_payload(term);
        size += field.normalized_text.capacity() + 1;
    }
    return size;
}
std::string store_name(Store store) {
    switch (store) {
        case Store::section: return "section";
        case Store::fact: return "fact";
        case Store::decision: return "decision";
        case Store::contract: return "contract";
        case Store::runbook: return "runbook";
    }
    return "?";
}
std::string row_name(const RowKey& key) { return store_name(key.first) + ":" + std::to_string(key.second); }

QueryRequest make_request(const Profile& profile, const EvalCase& item, const std::string& query) {
    QueryRequest request;
    request.query = query;
    request.terms = parse_terms(query);
    request.limit = item.limit > 0 ? item.limit : 8;
    request.per_doc_limit = item.per_doc_limit > 0 ? item.per_doc_limit : 2;
    apply_profile_query_expansion(profile, request);
    return request;
}

void add_store_members(QueryStage& query, Store store, const std::set<int>& ids) {
    for (int id : ids) query.members.emplace(store, id);
}
void validate_candidates(const QueryStage& query) {
    const auto& c = query.candidates;
    require(c.from_terms_index, query.label + " did not use the generated terms index");
    require(c.term_index_coverage_complete, query.label + " has incomplete terms-index coverage");
    require(c.discovered_row_count <= kMaxIndexedCandidateRows,
            query.label + " exceeded the indexed-candidate row bound");
    std::size_t sum = c.sections.size() + c.facts.size() + c.decisions.size() + c.contracts.size() + c.runbooks.size();
    require(sum == c.discovered_row_count, query.label + " candidate count does not match store IDs");
}

std::vector<QueryStage> make_queries(const RuntimeOptions& options) {
    std::vector<QueryStage> strict_queries, relaxed_queries;
    std::size_t metadata_pairs = 0;
    auto account = [&](const QueryStage& stage) {
        require(strict_queries.size() + relaxed_queries.size() < 256, "experiment exceeds 256 stages");
        metadata_pairs += stage.members.size();
        require(metadata_pairs <= 1048576, "experiment exceeds 1048576 candidate metadata pairs");
    };
    for (const auto& item : options.profile.eval_cases) {
        std::vector<std::pair<std::string, std::string>> prompts{{"base", item.query}};
        for (std::size_t i = 0; i < item.query_variants.size(); ++i)
            prompts.emplace_back("variant-" + std::to_string(i), item.query_variants[i]);
        for (const auto& [label, text] : prompts) {
            QueryRequest request = make_request(options.profile, item, text);
            const int strict = strict_required_group_matches(request);
            QueryStage stage{item.id + "/" + label + "/strict", request, strict, {}, {}, false};
            stage.candidates = load_generated_query_candidates(options, request.prepared_term_groups, strict);
            validate_candidates(stage);
            add_store_members(stage, Store::section, stage.candidates.sections);
            add_store_members(stage, Store::fact, stage.candidates.facts);
            add_store_members(stage, Store::decision, stage.candidates.decisions);
            add_store_members(stage, Store::contract, stage.candidates.contracts);
            add_store_members(stage, Store::runbook, stage.candidates.runbooks);
            account(stage);
            strict_queries.push_back(std::move(stage));
            if (can_relax_query(request)) {
                const int relaxed = relaxed_required_group_matches(request);
                QueryStage relaxed_stage{item.id + "/" + label + "/relaxed", request, relaxed, {}, {}, true};
                relaxed_stage.relaxed_stage = true;
                relaxed_stage.candidates = load_generated_query_candidates(
                    options, request.prepared_term_groups, relaxed);
                validate_candidates(relaxed_stage);
                add_store_members(relaxed_stage, Store::section, relaxed_stage.candidates.sections);
                add_store_members(relaxed_stage, Store::fact, relaxed_stage.candidates.facts);
                add_store_members(relaxed_stage, Store::decision, relaxed_stage.candidates.decisions);
                add_store_members(relaxed_stage, Store::contract, relaxed_stage.candidates.contracts);
                add_store_members(relaxed_stage, Store::runbook, relaxed_stage.candidates.runbooks);
                account(relaxed_stage);
                relaxed_queries.push_back(std::move(relaxed_stage));
            }
        }
    }
    require(!strict_queries.empty(), "profile has no eval requests");
    strict_queries.insert(strict_queries.end(), std::make_move_iterator(relaxed_queries.begin()),
                          std::make_move_iterator(relaxed_queries.end()));
    return strict_queries;
}

CandidateRows union_candidates(const std::vector<QueryStage>& queries,
                               const std::vector<std::size_t>& indexes) {
    CandidateRows out;
    out.from_terms_index = true;
    out.term_index_coverage_complete = true;
    for (auto index : indexes) {
        const auto& c = queries[index].candidates;
        out.sections.insert(c.sections.begin(), c.sections.end());
        out.facts.insert(c.facts.begin(), c.facts.end());
        out.decisions.insert(c.decisions.begin(), c.decisions.end());
        out.contracts.insert(c.contracts.begin(), c.contracts.end());
        out.runbooks.insert(c.runbooks.begin(), c.runbooks.end());
    }
    out.discovered_row_count = out.sections.size() + out.facts.size() + out.decisions.size() +
        out.contracts.size() + out.runbooks.size();
    return out;
}

void fetch_store(const RuntimeOptions& options, const CandidateRows& all, Store store,
                 std::map<RowKey, RowEntry>& pool, std::size_t& pool_payload) {
    CandidateRows one;
    one.from_terms_index = true;
    one.term_index_coverage_complete = true;
    const std::set<int>* ids = nullptr;
    switch (store) {
        case Store::section: ids = &all.sections; one.sections = *ids; break;
        case Store::fact: ids = &all.facts; one.facts = *ids; break;
        case Store::decision: ids = &all.decisions; one.decisions = *ids; break;
        case Store::contract: ids = &all.contracts; one.contracts = *ids; break;
        case Store::runbook: ids = &all.runbooks; one.runbooks = *ids; break;
    }
    if (ids->empty()) return;
    std::size_t seen = 0;
    // The callbacks below copy each immutable payload into a bounded typed pool.
    auto expected_id = ids->begin();
    auto expect_and_store = [&](const auto& row) {
        require(expected_id != ids->end(), store_name(store) + " reader returned too many callbacks");
        const int id = *expected_id++;
        require(id >= 0, store_name(store) + " reader returned an unsupported row identity");
        ++seen;
        RowKey key{store, id};
        RowEntry entry; entry.key = key;
        std::size_t payload = 0;
        if constexpr (std::is_same_v<std::decay_t<decltype(row)>, SectionRow>) {
            entry.section = row;
            payload = row_payload(entry.section);
        } else {
            entry.is_fact = true; entry.fact = row;
            payload = row_payload(entry.fact);
        }
        payload += sizeof(RowEntry) - (entry.is_fact ? sizeof(FactRow) : sizeof(SectionRow));
        require(pool.size() < kMaxIndexedCandidateRows, "row pool exceeds 20000 unique typed rows");
        require(payload <= 32U * 1024U * 1024U - pool_payload, "row pool exceeds 32 MiB estimated payload");
        require(pool.emplace(key, std::move(entry)).second, "duplicate typed row identity");
        pool_payload += payload;
    };
    if (store == Store::section) {
        for_each_generated_section_matching_terms(options, one, {}, 0,
            [&](const SectionRow& row) { expect_and_store(row); });
    } else {
        for_each_generated_fact_matching_terms(options, one, {}, 0, [&](const FactRow& row) { expect_and_store(row); });
    }
    require(seen == ids->size(), store_name(store) + " reader callback count mismatch");
}

constexpr std::size_t kWindowRows = 256;

std::vector<BatchData> make_batches(const std::vector<QueryStage>& queries,
                                    std::size_t batch_size,
                                    double& union_ms,
                                    std::size_t& total_pairs) {
    std::vector<BatchData> windows;
    const auto start = SteadyClock::now();
    for (std::size_t first = 0; first < queries.size();) {
        std::vector<std::size_t> indices;
        const bool relaxed = queries[first].relaxed_stage;
        std::size_t last = first;
        while (last < queries.size() && last < first + batch_size && queries[last].relaxed_stage == relaxed)
            indices.push_back(last++);
        const auto united = union_candidates(queries, indices);
        require(united.discovered_row_count <= kMaxIndexedCandidateRows, "batch candidate union exceeds 20000 rows");
        std::size_t batch_pairs = 0;
        for (auto q : indices) batch_pairs += queries[q].members.size();
        require(batch_pairs <= 65536, "batch row-query pairs exceed 65536");
        total_pairs += batch_pairs;
        BatchData window;
        auto initialize = [&] {
            window = {};
            window.first = first;
            window.query_indices = indices;
            window.candidates.from_terms_index = true;
            window.candidates.term_index_coverage_complete = true;
        };
        initialize();
        auto append = [&](Store store, const std::set<int>& ids) {
            for (const int id : ids) {
                std::set<int>* target = nullptr;
                switch (store) {
                    case Store::section: target = &window.candidates.sections; break;
                    case Store::fact: target = &window.candidates.facts; break;
                    case Store::decision: target = &window.candidates.decisions; break;
                    case Store::contract: target = &window.candidates.contracts; break;
                    case Store::runbook: target = &window.candidates.runbooks; break;
                }
                target->insert(id);
                ++window.candidates.discovered_row_count;
                for (auto q : indices) if (queries[q].members.contains({store, id})) ++window.pairs;
                if (window.candidates.discovered_row_count == kWindowRows) {
                    windows.push_back(std::move(window));
                    initialize();
                }
            }
        };
        append(Store::section, united.sections);
        append(Store::fact, united.facts);
        append(Store::decision, united.decisions);
        append(Store::contract, united.contracts);
        append(Store::runbook, united.runbooks);
        if (window.candidates.discovered_row_count) windows.push_back(std::move(window));
        first = last;
    }
    union_ms = elapsed_ms_since(start);
    return windows;
}

MatchScore score(const QueryStage& query, const RowEntry& row) {
    return row.is_fact ? score_fact_match(row.fact, query.request.prepared_term_groups, query.required)
                       : score_section_match(row.section, query.request.prepared_term_groups, query.required);
}

Json score_json(const MatchScore& score) {
    return {{"total", score.total}, {"matched_groups", score.matched_groups},
        {"components", score.components}, {"matched_fields", score.matched_fields},
        {"matched_group_indices", score.matched_group_indices}};
}

struct LaneResult {
    std::vector<ScoreDigest> digests;
    std::vector<std::size_t> calls;
    double wall_ms = 0;
    double cpu_ms = 0;
    double rowload_ms = 0;
    std::size_t rowloads = 0;
    std::vector<double> batch_wall_ms;
};

LaneResult run_lane(const RuntimeOptions& options, const std::vector<QueryStage>& queries,
                    std::vector<BatchData>& batches, bool candidate) {
    LaneResult lane; lane.digests.resize(queries.size()); lane.calls.resize(queries.size());
    for (auto& batch : batches) {
        std::map<RowKey, RowEntry> pool;
        std::size_t pool_payload = 0;
        const auto load_start = SteadyClock::now();
        for (Store store : {Store::section, Store::fact, Store::decision, Store::contract, Store::runbook})
            fetch_store(options, batch.candidates, store, pool, pool_payload);
        lane.rowload_ms += elapsed_ms_since(load_start);
        lane.rowloads += pool.size();
        require(pool.size() == batch.candidates.discovered_row_count,
                "typed row pool does not match this batch candidate union");
        batch.estimated_payload = pool_payload;
        // Scheduling/membership discovery is separate from both scorer lanes.
        std::vector<std::vector<const RowEntry*>> query_rows(batch.query_indices.size());
        std::vector<std::pair<const RowEntry*, unsigned>> row_masks;
        row_masks.reserve(pool.size());
        for (const auto& [key, row] : pool) {
            unsigned mask = 0;
            for (std::size_t slot = 0; slot < batch.query_indices.size(); ++slot) {
                if (queries[batch.query_indices[slot]].members.contains(key)) {
                    mask |= 1U << slot;
                    query_rows[slot].push_back(&row);
                }
            }
            require(mask != 0, "row has no eligible query");
            row_masks.emplace_back(&row, mask);
        }
        const auto score_start = SteadyClock::now();
        const auto cpu_start = std::clock();
        if (!candidate) {
            for (std::size_t slot = 0; slot < batch.query_indices.size(); ++slot) {
                const auto q = batch.query_indices[slot];
                for (const auto* row : query_rows[slot]) {
                    MatchScore result = score(queries[q], *row);
                    lane.digests[q].add(result); ++lane.calls[q];
                }
            }
        } else {
            for (const auto& [row, mask] : row_masks) {
                auto score_queries = [&] {
                    for (std::size_t slot = 0; slot < batch.query_indices.size(); ++slot) {
                        if (!(mask & (1U << slot))) continue;
                        const auto q = batch.query_indices[slot];
                        MatchScore result = score(queries[q], *row);
                        lane.digests[q].add(result); ++lane.calls[q];
                    }
                };
                if (row->is_fact) {
                    PreparedRowScope scope(row->fact);
                    score_queries();
                } else {
                    PreparedRowScope scope(row->section);
                    score_queries();
                }
            }
        }
        lane.batch_wall_ms.push_back(elapsed_ms_since(score_start));
        lane.wall_ms += elapsed_ms_since(score_start);
        const auto cpu_end = std::clock();
        if (cpu_start != std::clock_t(-1) && cpu_end != std::clock_t(-1))
            lane.cpu_ms += 1000.0 * (cpu_end - cpu_start) / CLOCKS_PER_SEC;
    }
    return lane;
}

void verify_scopes() {
    SectionRow section; section.path = "src/a.cpp"; section.document_id = "doc"; section.heading = "AlphaBeta";
    FactRow fact; fact.id = "ABC-123"; fact.title = "Runtime decision";
    require(prepared_batch_fields(section) == nullptr, "empty scope unexpectedly borrowed section fields");
    {
        PreparedRowScope outer(section);
        const auto* outer_fields = prepared_batch_fields(section);
        require(outer_fields && outer_fields == &outer.fields(), "section row was not borrowed by address");
        SectionRow equal_but_distinct = section;
        require(prepared_batch_fields(equal_but_distinct) == nullptr, "equal row at another address was borrowed");
        require(prepared_batch_fields(fact) == nullptr, "section scope leaked to fact type");
        {
            PreparedRowScope disabled;
            require(prepared_batch_fields(section) == nullptr, "empty nested scope did not disable borrowing");
        }
        require(prepared_batch_fields(section) == outer_fields, "nested scope did not restore the outer scope");
        {
            PreparedRowScope inner(fact);
            require(prepared_batch_fields(section) == nullptr && prepared_batch_fields(fact) == &inner.fields(),
                    "typed nested scope mismatch");
        }
        require(prepared_batch_fields(section) == outer_fields, "typed nested scope did not restore outer scope");
    }
    require(prepared_batch_fields(section) == nullptr, "scope did not restore empty state");

    auto groups = [](std::initializer_list<std::initializer_list<std::string>> values) {
        std::vector<std::vector<std::string>> raw;
        for (const auto& group : values) raw.emplace_back(group);
        return prepare_term_groups(raw);
    };
    auto prefix = groups({{"identi"}}); prefix[0][0].allow_prefix = true;
    const auto exact_groups = groups({{"AlphaBeta"}});
    const auto phrase_groups = groups({{"alpha beta"}});
    const auto path_groups = groups({{"src/a.cpp"}});
    const auto id_groups = groups({{"ABC-123"}});
    auto exact_score = [](const SectionRow& source, const PreparedTermGroups& terms) {
        MatchScore baseline, prepared;
        { PreparedRowScope empty; baseline = score_section_match(source, terms, 1); }
        { PreparedRowScope scope(source); prepared = score_section_match(source, terms, 1); }
        require(score_json(baseline) == score_json(prepared), "synthetic section scorer parity failed");
    };
    exact_score(section, exact_groups); exact_score(section, phrase_groups); exact_score(section, path_groups);
    SectionRow prefix_row = section; prefix_row.heading = "identity";
    exact_score(prefix_row, prefix);
    FactRow id_row = fact; id_row.identifier_terms = {"ABC-123"};
    MatchScore id_baseline, id_candidate;
    { PreparedRowScope empty; id_baseline = score_fact_match(id_row, id_groups, 1); }
    { PreparedRowScope scope(id_row); id_candidate = score_fact_match(id_row, id_groups, 1); }
    require(score_json(id_baseline) == score_json(id_candidate), "synthetic identifier scorer parity failed");

    // Exercise actual window partitioning across stores and the stage boundary.
    std::vector<QueryStage> window_queries(9);
    for (std::size_t q = 0; q < window_queries.size(); ++q) {
        auto& query = window_queries[q];
        query.relaxed_stage = q >= 5;
        for (int id = 0; id < 600; ++id) {
            if ((id + q) % 3 == 0) continue;
            query.candidates.sections.insert(id);
            query.members.emplace(Store::section, id);
            query.candidates.facts.insert(id);
            query.members.emplace(Store::fact, id);
        }
    }
    for (const auto size : {1U, 4U, 8U}) {
        double union_ms = 0; std::size_t pairs = 0;
        auto windows = make_batches(window_queries, size, union_ms, pairs);
        std::vector<std::vector<RowKey>> observed(window_queries.size());
        std::size_t seen_pairs = 0;
        for (const auto& window : windows) {
            require(window.candidates.discovered_row_count <= kWindowRows, "window size overflow");
            for (auto q : window.query_indices)
                require(window_queries[q].relaxed_stage == window_queries[window.first].relaxed_stage,
                        "window crosses strict/relaxed boundary");
            for (auto store : {Store::section, Store::fact}) {
                const auto& ids = store == Store::section ? window.candidates.sections : window.candidates.facts;
                for (int id : ids) for (auto q : window.query_indices) {
                    const RowKey key{store, id};
                    if (window_queries[q].members.contains(key)) observed[q].push_back(key);
                }
            }
            seen_pairs += window.pairs;
        }
        require(pairs == seen_pairs, "window pair accounting differs");
        for (std::size_t q = 0; q < window_queries.size(); ++q)
            require(observed[q] == std::vector<RowKey>(window_queries[q].members.begin(), window_queries[q].members.end()),
                    "window split lost, duplicated or reordered a row");
    }

    // Exercise row-major schedules over several bounded batch sizes.
    const std::array<std::size_t, 3> sizes{1, 4, 8};
    std::array<std::set<RowKey>, 9> memberships;
    for (std::size_t q = 0; q < memberships.size(); ++q) {
        memberships[q].emplace(Store::section, static_cast<int>(q % 5));
        memberships[q].emplace(Store::fact, static_cast<int>(q % 3));
        memberships[q].emplace(Store::section, static_cast<int>((q + 2) % 5));
    }
    for (const std::size_t size : sizes) {
        std::vector<std::vector<RowKey>> major(memberships.size()), row_major(memberships.size());
        for (std::size_t q = 0; q < memberships.size(); ++q)
            major[q].assign(memberships[q].begin(), memberships[q].end());
        for (std::size_t first = 0; first < memberships.size(); first += size) {
            const std::size_t last = std::min(memberships.size(), first + size);
            std::set<RowKey> union_keys;
            for (std::size_t q = first; q < last; ++q)
                union_keys.insert(memberships[q].begin(), memberships[q].end());
            for (const RowKey& key : union_keys)
                for (std::size_t q = first; q < last; ++q)
                    if (memberships[q].contains(key)) row_major[q].push_back(key);
        }
        require(major == row_major, "synthetic membership schedule changed query/store/id order");
    }
}

int benchmark(int argc, char** argv) {
    verify_scopes();
    if (argc == 1) {
        std::cout << Json({{"ok", true}, {"synthetic", true}, {"schedule_batch_sizes", {1, 4, 8}}}).dump() << '\n';
        return 0;
    }
    if (argc != 7) {
        std::cerr << "usage: shin-row-batch-bench ROOT OUT PROFILEDIR PROFILE baseline-first|candidate-first BATCHSIZE\n";
        return 2;
    }
    const std::string order = argv[5];
    require(order == "baseline-first" || order == "candidate-first", "invalid order");
    const std::size_t batch_size = std::stoull(argv[6]);
    require(batch_size > 0 && batch_size <= 8, "batch size must be 1..8");
    RuntimeOptions options;
    options.root = fs::canonical(argv[1]); options.out_root = fs::canonical(argv[2]);
    options.profile_dir = fs::canonical(argv[3]); options.profile = make_profile(argv[4], options.profile_dir);
    options.force_index = true;
    RetrievalSession session(options, true);
    const auto discovery_start = SteadyClock::now();
    auto queries = make_queries(session.options());
    const double candidate_discovery_ms = elapsed_ms_since(discovery_start);
    double union_ms = 0; std::size_t pairs = 0;
    auto batches = make_batches(queries, batch_size, union_ms, pairs);
    Json baseline, candidate;
    for (bool do_candidate : {order == "candidate-first", order != "candidate-first"}) {
        LaneResult lane = run_lane(session.options(), queries, batches, do_candidate);
        Json per_query = Json::array();
        for (std::size_t i = 0; i < queries.size(); ++i) {
            require(lane.calls[i] == queries[i].members.size(), "lane omitted or duplicated query candidates");
            per_query.push_back({{"label", queries[i].label}, {"calls", lane.calls[i]}, {"digest", lane.digests[i].text()}});
        }
        Json run{{"wall_ms", lane.wall_ms}, {"cpu_ms", lane.cpu_ms}, {"per_query", per_query},
            {"per_batch_wall_ms", lane.batch_wall_ms},
            {"rowload_ms", lane.rowload_ms}, {"rowloads", lane.rowloads}};
        if (do_candidate) candidate = std::move(run); else baseline = std::move(run);
    }
    require(candidate["per_query"] == baseline["per_query"], "candidate per-query score digest/call parity failed");
    std::size_t max_field_working_bytes = 0;
    // Exact pair-level oracle shares candidate fields across each row's eligible queries.
    for (const auto& batch : batches) {
        std::map<RowKey, RowEntry> pool;
        std::size_t pool_payload = 0;
        for (Store store : {Store::section, Store::fact, Store::decision, Store::contract, Store::runbook})
            fetch_store(session.options(), batch.candidates, store, pool, pool_payload);
        require(pool.size() == batch.candidates.discovered_row_count, "oracle batch row count mismatch");
        for (const auto& [key, row] : pool) {
            if (row.is_fact) {
                PreparedRowScope candidate_scope(row.fact);
                for (auto q : batch.query_indices) if (queries[q].members.contains(key)) {
                    const MatchScore actual = score(queries[q], row);
                    MatchScore expected;
                    { PreparedRowScope empty_scope; expected = score(queries[q], row); }
                    require(score_json(expected) == score_json(actual),
                            queries[q].label + " exact score differs for " + row_name(key));
                }
                max_field_working_bytes = std::max(max_field_working_bytes, fields_payload(candidate_scope.fields()));
            } else {
                PreparedRowScope candidate_scope(row.section);
                for (auto q : batch.query_indices) if (queries[q].members.contains(key)) {
                    const MatchScore actual = score(queries[q], row);
                    MatchScore expected;
                    { PreparedRowScope empty_scope; expected = score(queries[q], row); }
                    require(score_json(expected) == score_json(actual),
                            queries[q].label + " exact score differs for " + row_name(key));
                }
                max_field_working_bytes = std::max(max_field_working_bytes, fields_payload(candidate_scope.fields()));
            }
        }
    }
    struct rusage usage{}; const bool have_rss = getrusage(RUSAGE_SELF, &usage) == 0;
#if defined(__APPLE__)
    const std::uint64_t peak_rss_bytes = have_rss ? static_cast<std::uint64_t>(usage.ru_maxrss) : 0;
#else
    const std::uint64_t peak_rss_bytes = have_rss ? static_cast<std::uint64_t>(usage.ru_maxrss) * 1024ULL : 0;
#endif
    Json batch_json = Json::array();
    for (const auto& batch : batches) batch_json.push_back({{"first_query_index", batch.first},
        {"query_count", batch.query_indices.size()}, {"rows", batch.candidates.discovered_row_count}, {"pairs", batch.pairs},
        {"estimated_pool_payload_bytes", batch.estimated_payload}});
    Json result{{"schema_version", "shin.row-batch-scoring-experiment.v1"}, {"ok", true}, {"order", order},
        {"batch_size", batch_size}, {"row_window_limit", kWindowRows}, {"queries", queries.size()},
        {"sum_batch_unique_rows", baseline["rowloads"]},
        {"row_query_pairs", pairs}, {"max_field_working_bytes_estimate", max_field_working_bytes}, {"candidate_discovery_ms", candidate_discovery_ms},
        {"union_ms", union_ms}, {"baseline", baseline},
        {"candidate", candidate}, {"batches", batch_json},
        {"process_peak_rss_bytes", have_rss ? Json(peak_rss_bytes) : Json(nullptr)},
        {"boundary", "Optional scorer-stage experiment only. Strict and relaxed indexed candidate sets are independent hypothetical stages, not adaptive retrieval routes. Rows stream in sorted windows of at most 256 per query batch, each with a 32 MiB payload ceiling. Rowpool estimate counts row object and owned string/vector capacities, not allocator/container overhead. No full-Brief quality, refresh speedup, or installed-runtime claim."}};
    std::cout << result.dump(2) << '\n';
    return 0;
}
}

int main(int argc, char** argv) {
    try { return benchmark(argc, argv); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
