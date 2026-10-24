#include "agent_context_pack.h"
#include "context_internal.h"
#include "generated_io.h"
#include "generated_row_decoder.h"
#include "generated_snapshot.h"
#include "retrieval_session.h"

#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <thread>

namespace fs = std::filesystem;

namespace shin::context {
namespace {

class TempDirectory {
public:
    explicit TempDirectory(const fs::path& parent) {
        static std::atomic<unsigned long long> sequence{0};
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = parent / ("shin-warm-snapshot-" + std::to_string(stamp) + "." +
                          std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)));
        fs::create_directories(path_);
    }

    ~TempDirectory() {
        std::error_code error;
        fs::remove_all(path_, error);
    }

    const fs::path& path() const noexcept { return path_; }

private:
    fs::path path_;
};

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error("warm snapshot test: " + message);
}

void expect(bool condition, const std::string& message) {
    if (!condition) fail(message);
}

template <typename Callback>
void expect_cli_error(Callback&& callback, const std::string& expected_code,
                      const std::string& label) {
    try {
        callback();
    } catch (const CliError& error) {
        expect(error.code() == expected_code,
               label + " returned " + error.code() + " instead of " + expected_code);
        return;
    } catch (const std::exception& error) {
        fail(label + " returned a non-CLI error: " + error.what());
    }
    fail(label + " did not reject the input");
}

void write_text(const fs::path& path, std::string_view content) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) fail("cannot create fixture file " + path.string());
    output.write(content.data(), static_cast<std::streamsize>(content.size()));
    if (!output) fail("cannot write fixture file " + path.string());
}

Profile fixture_profile() {
    Profile profile;
    profile.id = "warm-snapshot";
    profile.source_label = "warm snapshot fixture";
    profile.config_hash = "warm-snapshot-test-profile-v1";
    profile.root_files = {"README.md"};
    profile.source_fact_roots = {"src"};
    profile.source_fact_extensions = {".cpp"};
    profile.source_fact_extractors = {"language"};
    profile.compound_query_first_term_scope = true;
    return profile;
}

RuntimeOptions make_fixture_options(const fs::path& base) {
    RuntimeOptions options;
    options.root = base / "root";
    options.out_root = base / "index";
    options.profile = fixture_profile();
    options.force_index = true;
    fs::create_directories(options.root);
    return options;
}

void write_generation_a(const RuntimeOptions& options) {
    write_text(options.root / "README.md",
               "# Warm Snapshot Fixture\n\n"
               "The warm snapshot fixture documents immutable retrieval evidence.\n");
    write_text(options.root / "docs/runtime/overview.md",
               "# Snapshot Isolation\n\n"
               "WarmRevisionOne keeps snapshot isolation and deterministic query cursors.\n"
               "The runtime uses a memory generation while the publisher prepares a successor.\n");
    write_text(options.root / "docs/runtime/publisher.md",
               "# Concurrent Publisher\n\n"
               "The publisher commits a snapshot generation without changing resident evidence.\n"
               "Isolation and lifecycle checks protect concurrent retrieval.\n");
    write_text(options.root / "docs/contracts/runtime.md",
               "# Runtime Contract\n\n"
               "A memory view serves snapshot evidence and the active generation remains verifiable.\n");
    write_text(options.root / "src/Runtime.cpp",
               "class RuntimeSnapshot { public: void publish_generation(); };\n");
}

void write_generation_b(const RuntimeOptions& options) {
    write_text(options.root / "docs/runtime/overview.md",
               "# Snapshot Isolation\n\n"
               "WarmRevisionTwo keeps snapshot isolation and deterministic query cursors.\n"
               "The successor generation is active after concurrent publication.\n");
    write_text(options.root / "docs/runtime/publisher.md",
               "# Concurrent Publisher\n\n"
               "The publisher commits WarmRevisionTwo while old memory evidence remains resident.\n"
               "Isolation and lifecycle checks protect concurrent retrieval.\n");
}

void publish_files(const RuntimeOptions& options, const std::map<std::string, std::string>& files) {
    shin::generated_io::IndexLock writer_lock(
        options.out_root, shin::generated_io::LockMode::exclusive,
        shin::generated_io::LockTarget::writer);
    shin::generated_io::recover_snapshot(options.out_root, kGeneratedDir);
    shin::generated_io::publish_snapshot(options.out_root, kGeneratedDir, files);
}

void publish_generation(const RuntimeOptions& options) {
    const GeneratedOutput generated = generate_outputs(options.profile, options.root);
    publish_files(options, generated.files);
}

QueryRequest query(const RuntimeOptions& options, const std::vector<std::string>& args,
                   int limit = 6, int per_document = 2) {
    return make_command_query_request(options.profile, args, limit, per_document);
}

TaskRequest pack_request(const RuntimeOptions& options) {
    return parse_task_request(
        {
            {"schema_version", "shin.task-request.v1"},
            {"request_id", "warm-snapshot-pack"},
            {"evidence_types", {"section", "fact"}},
            {"budget", {{"max_evidence_bytes", 16384}, {"max_records", 16},
                        {"per_query_records", 6}, {"per_document_records", 2}}},
            {"task", {{"objective", "Validate warm snapshot retrieval."},
                      {"scope", {{{"id", "runtime-scope"},
                                   {"query", "docs/runtime snapshot isolation"}}}},
                      {"must", {{{"id", "publisher"},
                                  {"query", "snapshot generation publisher isolation concurrent"}}}},
                      {"should", {{{"id", "contract"},
                                    {"query", "memory view active generation"}}}}}}
        },
        options.profile);
}

Json render_pack(const RuntimeOptions& options, const TaskRequest& request,
                 const RetrievalSession& session, int jobs) {
    const AgentContextPack pack = execute_agent_context_pack(options, request, session, jobs);
    Json rendered =
        render_agent_context_pack(options, {PackProjection::full, false, true}, request, session, pack);
    rendered.at("execution").erase("jobs");
    return rendered;
}

void expect_snapshot_only_freshness(const RetrievalSession& session) {
    const IndexFreshness& freshness = session.freshness();
    expect(freshness.status == "not-assessed", "snapshot-only session must not claim current freshness");
    expect(!freshness.current && !freshness.fully_verified,
           "snapshot-only session must remain freshness-unassessed");
}

void expect_current_freshness(const RetrievalSession& session) {
    const IndexFreshness& freshness = session.freshness();
    expect(freshness.current && freshness.fully_verified,
           "require-current memory session must admit only a current route");
}

void expect_equal(const Json& left, const Json& right, const std::string& label) {
    if (left != right) fail(label + " differs");
}

Json brief_result_digest(const BriefResult& result) {
    Json units = Json::array();
    for (const auto& [unit, groups] : result.planner_unit_coverage) {
        units.push_back({{"path", unit.path}, {"whole_document", unit.whole_document},
                         {"evidence_kind", unit.evidence_kind}, {"verification_level", unit.verification_level},
                         {"authority_level", unit.authority_level}, {"status", unit.status},
                         {"record_key", unit.record_key}, {"groups", groups}});
    }
    Json sections = Json::array();
    for (const auto& [path, hits] : result.section_hits_by_doc) {
        for (const ContextHit& hit : hits) {
            sections.push_back({{"path", path}, {"hit", section_hit_json(hit)}});
        }
    }
    auto facts = [](const std::vector<FactHit>& hits) {
        Json rows = Json::array();
        for (const FactHit& hit : hits) rows.push_back(fact_hit_json(hit));
        return rows;
    };
    return {
        {"planner_attempted", result.planner_attempted},
        {"query_planner_used", result.query_planner_used},
        {"candidate_rows", result.candidate_rows},
        {"planner_covered_groups", result.planner_covered_groups},
        {"planner_selected_documents", result.planner_selected_documents},
        {"planner_units", std::move(units)},
        {"sections", std::move(sections)},
        {"decisions", facts(result.decisions)},
        {"contracts", facts(result.contracts)},
        {"runbooks", facts(result.runbooks)},
        {"facts", facts(result.facts)}
    };
}

void assert_disk_memory_parity(const RuntimeOptions& options,
                               const std::shared_ptr<const GeneratedSnapshot>& snapshot) {
    RetrievalSession disk(options, RetrievalSessionOpenMode::verified_snapshot);
    RetrievalSession memory(snapshot, false);
    expect_snapshot_only_freshness(disk);
    expect_snapshot_only_freshness(memory);

    const QueryRequest search = query(options, {"snapshot", "isolation"});
    const QueryRequest context = query(options, {"memory", "generation"}, 8, 3);
    const QueryRequest normal_brief = query(options, {"snapshot", "isolation"});
    const QueryRequest scoped_brief = query(options, {"docs/runtime", "snapshot", "isolation"});
    const QueryRequest planned_brief = query(
        options, {"snapshot", "generation", "publisher", "isolation", "concurrent",
                  "lifecycle", "resident", "evidence"});

    expect_equal(disk.search(search), memory.search(search), "search parity");
    expect_equal(disk.context(context), memory.context(context), "context parity");
    expect_equal(disk.brief(normal_brief), memory.brief(normal_brief), "normal brief parity");
    const BriefExecution disk_scoped = disk.brief_execution(scoped_brief);
    const BriefExecution memory_scoped = memory.brief_execution(scoped_brief);
    expect(disk_scoped.retrieval.directory_owner_scope_used ||
               disk_scoped.retrieval.document_owner_scope_used,
           "scoped brief did not use a scoped retrieval branch");
    expect(memory_scoped.retrieval.directory_owner_scope_used ||
               memory_scoped.retrieval.document_owner_scope_used,
           "memory scoped brief did not use a scoped retrieval branch");
    expect_equal(disk.brief(scoped_brief), memory.brief(scoped_brief), "scoped brief parity");
    expect_equal(disk.brief(planned_brief), memory.brief(planned_brief), "planned brief parity");
    const BriefResult disk_planned =
        collect_brief_from_index_document_coverage_plan(
            disk.options(), planned_brief, false, relaxed_required_group_matches(planned_brief), false);
    const BriefResult memory_planned =
        collect_brief_from_index_document_coverage_plan(
            memory.options(), planned_brief, false, relaxed_required_group_matches(planned_brief), false);
    expect(disk_planned.query_planner_used && memory_planned.query_planner_used,
           "direct planned retrieval did not activate the coverage planner");
    expect_equal(brief_result_digest(disk_planned), brief_result_digest(memory_planned),
                 "direct planned brief parity");

    const TaskRequest request = pack_request(options);
    const Json disk_serial = render_pack(options, request, disk, 1);
    const Json memory_serial = render_pack(options, request, memory, 1);
    const Json memory_parallel = render_pack(options, request, memory, 4);
    expect_equal(disk_serial, memory_serial, "serial pack parity");
    expect_equal(memory_serial, memory_parallel, "serial/parallel pack parity");
}

void assert_independent_cursors(const RuntimeOptions& options,
                                const std::shared_ptr<const GeneratedSnapshot>& snapshot) {
    RetrievalSession session(snapshot, false);
    const QueryRequest request = query(options, {"snapshot", "isolation"});
    const Json expected = session.search(request);
    std::vector<std::future<Json>> futures;
    futures.reserve(16);
    for (int index = 0; index < 16; ++index) {
        futures.push_back(std::async(std::launch::async, [&session, &request] {
            return session.search(request);
        }));
    }
    for (auto& future : futures) {
        expect_equal(expected, future.get(), "independent concurrent cursor result");
    }
}

void assert_snapshot_survives_missing_artifacts(
    const RuntimeOptions& options, const std::shared_ptr<const GeneratedSnapshot>& snapshot) {
    RetrievalSession before(snapshot, false);
    const QueryRequest search = query(options, {"warmrevisionone"});
    const QueryRequest context = query(options, {"memory", "generation"}, 8, 3);
    const QueryRequest scoped_brief = query(options, {"docs/runtime", "snapshot", "isolation"});
    const QueryRequest planned_brief = query(
        options, {"snapshot", "generation", "publisher", "isolation", "concurrent",
                  "lifecycle", "resident", "evidence"});
    const QueryRequest document_owner =
        query(options, {"docs/runtime/publisher.md", "snapshot"});
    const Json baseline_search = before.search(search);
    const Json baseline_context = before.context(context);
    const Json baseline_scoped_brief = before.brief(scoped_brief);
    const Json baseline_planned_brief = before.brief(planned_brief);
    const auto baseline_document_candidates =
        collect_brief_from_index_document_coverage_plan(
            before.options(), planned_brief, true, relaxed_required_group_matches(planned_brief), false);
    expect(std::any_of(baseline_document_candidates.planner_unit_coverage.begin(),
                       baseline_document_candidates.planner_unit_coverage.end(),
                       [](const auto& unit) { return unit.first.whole_document; }),
           "document-candidate fixture did not qualify any owner");
    auto capped_groups = load_generated_query_group_candidates(before.options(), planned_brief.prepared_term_groups);
    capped_groups.front() = CandidateRows{};
    capped_groups.front().term_index_coverage_complete = true;
    capped_groups.front().discovered_row_count = kMaxIndexedCandidateRows + 1;
    expect_equal(brief_result_digest(baseline_document_candidates),
                 brief_result_digest(collect_brief_from_index_document_coverage_plan(
                     before.options(), planned_brief, true, relaxed_required_group_matches(planned_brief),
                     false, &capped_groups)), "document recovery from a discarded group");
    const Json baseline_pack = render_pack(options, pack_request(options), before, 1);
    const BriefResult baseline_document_owner =
        collect_brief_from_index_document_owner_scope(before.options(), document_owner, nullptr);
    expect(baseline_document_owner.document_owner_scope_used,
           "document-owner fallback did not resolve the loaded manifest");

    const fs::path generated = options.out_root / kGeneratedDir;
    const auto terms = generated / "terms.lex";
    const auto original = read_file(terms);
    write_text(terms, "truncated");
    expect(snapshot->file(terms) == original, "resident bytes changed after in-place truncation");
    write_text(terms, original);
    const fs::path hidden = options.out_root / "generated-hidden-for-warm-test";
    fs::rename(generated, hidden);
    try {
        RetrievalSession after(snapshot, false);
        expect_equal(baseline_search, after.search(search), "artifact-hidden search");
        expect_equal(baseline_context, after.context(context), "artifact-hidden context");
        expect_equal(baseline_scoped_brief, after.brief(scoped_brief), "artifact-hidden scoped brief");
        expect_equal(baseline_planned_brief, after.brief(planned_brief), "artifact-hidden planned brief");
        expect_equal(brief_result_digest(baseline_document_candidates),
                     brief_result_digest(collect_brief_from_index_document_coverage_plan(
                         after.options(), planned_brief, true,
                         relaxed_required_group_matches(planned_brief), false)),
                     "artifact-hidden document row index");
        expect_equal(brief_result_digest(baseline_document_candidates),
                     brief_result_digest(collect_brief_from_index_document_coverage_plan(
                         after.options(), planned_brief, true, relaxed_required_group_matches(planned_brief),
                         false, &capped_groups)), "artifact-hidden document recovery");
        expect_equal(baseline_pack, render_pack(options, pack_request(options), after, 1),
                     "artifact-hidden pack");
        const BriefResult after_document_owner =
            collect_brief_from_index_document_owner_scope(after.options(), document_owner, nullptr);
        expect_equal(brief_result_digest(baseline_document_owner),
                     brief_result_digest(after_document_owner),
                     "artifact-hidden document-owner fallback");
    } catch (...) {
        fs::rename(hidden, generated);
        throw;
    }
    fs::rename(hidden, generated);
}

void assert_load_rejections(const fs::path& parent) {
    TempDirectory temporary(parent);
    RuntimeOptions options = make_fixture_options(temporary.path());
    write_generation_a(options);
    publish_generation(options);

    GeneratedOutput old_generator = generate_outputs(options.profile, options.root);
    const std::string old_manifest_path = (kGeneratedDir / "manifest.json").generic_string();
    Json old_manifest = parse_json_text(old_generator.files.at(old_manifest_path), old_manifest_path);
    old_manifest["generator_version"] = "shin-generator-v18-document-candidates";
    old_generator.files[old_manifest_path] = old_manifest.dump();
    publish_files(options, old_generator.files);
    expect_cli_error([&] { (void)GeneratedSnapshot::load(options); },
                     "snapshot-route-mismatch", "pre-redaction generator");
    publish_generation(options);

    RuntimeOptions wrong_profile = options;
    wrong_profile.profile.id = "wrong-warm-snapshot-profile";
    expect_cli_error([&] { (void)GeneratedSnapshot::load(wrong_profile); },
                     "snapshot-route-mismatch", "wrong profile");

    SnapshotLoadLimits too_small;
    too_small.maximum_payload_bytes = 1;
    too_small.maximum_file_bytes = 1;
    expect_cli_error([&] { (void)GeneratedSnapshot::load(options, too_small); },
                     "snapshot-memory-budget-exceeded", "payload byte cap");

    GeneratedOutput incomplete = generate_outputs(options.profile, options.root);
    const std::string runbooks = (kGeneratedDir / "runbooks.jsonl").generic_string();
    const std::string runbook_offsets = (kGeneratedDir / "runbooks.index.tsv").generic_string();
    incomplete.files.erase(runbooks);
    incomplete.files.erase(runbook_offsets);
    const std::string manifest_path = (kGeneratedDir / "manifest.json").generic_string();
    Json manifest = parse_json_text(incomplete.files.at(manifest_path), manifest_path);
    auto remove_declared_paths = [&](Json& rows) {
        Json retained = Json::array();
        for (const Json& row : rows) {
            const std::string path = json_string_value(row, "path");
            if (path != runbooks && path != runbook_offsets) retained.push_back(row);
        }
        rows = std::move(retained);
    };
    remove_declared_paths(manifest.at("outputs"));
    remove_declared_paths(manifest.at("generated_file_integrity").at("files"));
    incomplete.files[manifest_path] = manifest.dump();
    publish_files(options, incomplete.files);
    expect_cli_error([&] { (void)GeneratedSnapshot::load(options); }, "index-unavailable",
                     "internally matching snapshot missing a required store");

    publish_generation(options);

    const fs::path sections = options.out_root / kGeneratedDir / "sections.jsonl";
    std::ofstream corrupt(sections, std::ios::binary | std::ios::app);
    corrupt << "corrupt";
    corrupt.close();
    expect_cli_error([&] { (void)GeneratedSnapshot::load(options); },
                     "snapshot-integrity-failed", "corrupt snapshot");
}

void assert_resident_generation_does_not_block_publish(
    const RuntimeOptions& options, const std::shared_ptr<const GeneratedSnapshot>& old_snapshot) {
    auto old_session = std::make_unique<RetrievalSession>(old_snapshot, false);
    const QueryRequest old_query = query(options, {"warmrevisionone"});
    expect(!old_session->search(old_query).at("hits").empty(), "old snapshot fixture missing revision one");

    write_generation_b(options);
    std::promise<void> completion;
    std::future<void> published = completion.get_future();
    std::thread publisher([&] {
        try {
            publish_generation(options);
            completion.set_value();
        } catch (...) {
            completion.set_exception(std::current_exception());
        }
    });

    const auto ready = published.wait_for(std::chrono::seconds(3));
    if (ready != std::future_status::ready) {
        old_session.reset();
    }
    if (publisher.joinable()) publisher.join();
    expect(ready == std::future_status::ready,
           "publisher remained blocked by an idle resident memory generation");
    published.get();

    expect(!old_session->search(old_query).at("hits").empty(),
           "old resident session did not retain its generation after publication");
    const auto new_snapshot = GeneratedSnapshot::load(options);
    RetrievalSession new_session(new_snapshot, false);
    expect(new_session.search(query(options, {"warmrevisionone"})).at("hits").empty(),
           "new generation still returned old-only evidence");
    expect(!new_session.search(query(options, {"warmrevisiontwo"})).at("hits").empty(),
           "new generation did not return successor evidence");
}

void assert_stale_source_admission(const std::shared_ptr<const GeneratedSnapshot>& snapshot) {
    expect_cli_error([&] { (void)RetrievalSession(snapshot, true); }, "stale-index",
                     "stale source require-current admission");
}

void assert_changed_generation_admission(
    const RuntimeOptions& options, const std::shared_ptr<const GeneratedSnapshot>& snapshot) {
    expect_cli_error([&] { (void)RetrievalSession(snapshot, true); },
                     "snapshot-generation-not-current",
                     "changed active generation require-current admission");

    const auto current_snapshot = GeneratedSnapshot::load(options);
    RetrievalSession current(current_snapshot, true);
    expect_current_freshness(current);
}

void assert_lexical_word_match_equivalence() {
    const std::string scattered = std::string(120, ' ') + "middle almanac granite elder titanium orange";
    FieldMatchInput absent("summary", scattered, 20, true);
    expect(field_match_kind(absent, prepare_match_term("magneto")) == LexicalMatchKind::none,
           "scattered letters are not lexical evidence");
    expect(!absent.lexical_ready, "impossible single-word field built a complete vocabulary");

    const auto phrase = prepare_term_groups({{"session context"}}).front().front();
    const std::string prose = std::string(100, ' ') + "session context is preserved";
    FieldMatchInput prose_field("summary", prose, 20, true);
    expect(field_match_kind(prose_field, phrase) == LexicalMatchKind::exact,
           "compound phrase was rejected across whitespace");
    std::vector<std::string> texts{
        "AlphaBeta", "alpha_beta", "root.alpha/beta", "foo/bar/baz", "alpha beta", "v/3", "C++20",
        "source-fact:bff/src/SceneLabV3Module.java:42:method", std::string("alpha\xce\xb2") + "beta"
    };
    // Every byte boundary, including NUL/non-ASCII; compact identifiers, paths,
    // overlap-heavy words and 96-byte fast-path transition keep vocabulary parity.
    for (unsigned c = 0; c < 256; ++c)
        texts.push_back(std::string(100, ' ') + "Alpha" + static_cast<char>(c) +
                        "Beta HTTPRequest foo/bar-baz AAAAAABaaaaab C++20 cache2Node");
    texts.push_back(std::string(128, 'a'));
    texts.push_back(std::string(129, 'a'));
    texts.push_back("folder/" + std::string(160, 'a') + "/name.cpp");
    texts.push_back("Alpha Beta Gamma delta epsilon AlphaBetaGamma");
    texts.push_back("aaaaabaaaaab/aaabaaab alternatingABABAB marker");
    for (const int padding : {0, 85, 86, 87}) texts.push_back("alpha beta" + std::string(padding, ' '));
    std::string large;
    for (int index = 0; index < 400; ++index)
        large += " AlphaBeta root.alpha/beta C++20 delta_marker item" + std::to_string(index);
    texts.push_back(std::move(large));
    for (const auto& text : texts) {
        const auto reference = lexical_match_terms(text);
        const auto backing = "outsidePrefix" + text + "outsideSuffix";
        expect(lexical_match_terms(std::string_view(backing).substr(13, text.size())) == reference,
               "lexical extraction read outside its non-terminated view");
        auto unsorted = lexical_match_terms(text, false);
        // A later extraction must not overwrite strings returned from the local arena.
        (void)lexical_match_terms("replacement words and TemporaryScope", false);
        std::sort(unsorted.begin(), unsorted.end());
        expect(unsorted == reference, "unsorted lexical extraction changed vocabulary or storage ownership");
        expect(std::adjacent_find(unsorted.begin(), unsorted.end()) == unsorted.end(),
               "lexical extraction must remove duplicate terms before indexing");
        for (const auto* name : {"path", "summary"}) {
            FieldMatchInput prepared(name, text, 14, true);
            prepared.lexical_terms = reference;
            prepared.lexical_ready = true;
            // Include whole paths beyond vocabulary limits and multiword phrases
            // whose confirmation uses normalized text rather than term lookup.
            for (const auto& query : {text, std::string("alpha beta gamma"),
                                     std::string("gamma delta epsilon"), std::string("missing compound phrase")}) {
                const auto groups = prepare_term_groups({{query}});
                for (const auto& group : groups) for (const auto& term : group) {
                    FieldMatchInput cold(name, text, 14, true);
                    expect(field_match_kind(prepared, term) == field_match_kind(cold, term),
                           "prepared vocabulary changed path/phrase confirmation");
                }
            }
        }
        std::set<std::string> words{"alpha", "beta", "alphabeta", "foobar", "barbaz", "v3", "source", "fact", "method", "absent"};
        for (const auto& word : reference) {
            if (index_term_has_signal(word)) words.insert(word);
        }
        FieldMatchInput reused("path", text, 14, true);
        reused.lexical_terms = reference;
        reused.lexical_ready = true;
        for (const auto& word : words) {
            FieldMatchInput field("path", text, 14, true);
            const bool expected = std::binary_search(reference.begin(), reference.end(), word);
            const bool actual = field_match_kind(field, prepare_match_term(word)) == LexicalMatchKind::exact;
            expect(actual == expected, "lexical matcher differs from indexed vocabulary: " + text + " / " + word);
            expect(field_match_kind(reused, prepare_match_term(word)) ==
                       (actual ? LexicalMatchKind::exact : LexicalMatchKind::none),
                   "prepared field vocabulary changed exact matching");
            if (can_prefix_complete_lookup_term(word)) {
                auto prefix_term = prepare_match_term(word.substr(0, 4));
                prefix_term.allow_prefix = true;
                FieldMatchInput prefix_field("summary", text, 20, true);
                const bool prefix_expected = std::any_of(reference.begin(), reference.end(), [&](const auto& value) {
                    return value == prefix_term.raw || (can_prefix_complete_lookup_term(value) && starts_with(value, prefix_term.raw));
                });
                const auto cold_prefix = field_match_kind(prefix_field, prefix_term);
                expect((cold_prefix != LexicalMatchKind::none) == prefix_expected,
                       "plain-word prefilter changed prefix evidence");
                expect(field_match_kind(reused, prefix_term) == cold_prefix,
                       "prepared field vocabulary changed exact/prefix precedence");
            }
        }
    }
}

void assert_section_publication(const fs::path& parent) {
    TempDirectory temporary(parent);
    RuntimeOptions options = make_fixture_options(temporary.path());
    options.profile.markdown_chunk_lines = 0;
    write_generation_a(options);
    std::string body;
    for (int i = 0; i < 600; ++i) body += "ordinary filler ";
    body += "LateResidentNeedle tailalpha";
    const auto body_path = options.root / "docs/full-body.md";
    write_text(body_path, "# Full body\n" + body + "\n");
    std::string secret = "sk-";
    for (int i = 0; i < 16; ++i) secret += "Q7x9";
    write_text(options.root / "docs/secret.md", "# Cutoff\n" + std::string(263, 'p') + " " + secret + "\n");
    publish_generation(options);
    const auto snapshot = GeneratedSnapshot::load(options);
    Fnv1a64Hasher streamed;
    streamed.update("snapshot");
    streamed.update("\n");
    streamed.update("verification");
    expect(streamed.digest() == fnv1a64("snapshot\nverification"), "streaming hash parity");
    expect(Fnv1a64Hasher().digest() == fnv1a64(""), "empty streaming hash parity");
    for (const auto& [path, content] : generate_outputs(options.profile, options.root).files) {
        (void)path;
        expect(content.find(secret.substr(0, 8)) == std::string::npos,
               "publication retained a credential prefix");
    }

    const auto request = parse_task_request({
        {"schema_version", "shin.task-request.v1"}, {"request_id", "resident-body"},
        {"task", {{"objective", "Verify late body evidence"}, {"must", {
            {{"id", "late"}, {"query", "LateResidentNeedle"}, {"require_full_coverage", true}},
            {{"id", "same"}, {"query", "LateResidentNeedle"}, {"require_full_coverage", true}}
        }}}},
        {"evidence_types", {"section"}},
        {"budget", {{"max_evidence_bytes", 4000}, {"max_records", 1},
                    {"per_query_records", 1}, {"per_document_records", 1}}}
    }, options.profile);
    const auto late = query(options, {"LateResidentNeedle"});
    RetrievalSession resident(snapshot, false);
    const auto context = resident.context(late);
    const auto pack = render_pack(options, request, resident, 1);
    expect(context.at("sections").size() == 1, "resident late-body evidence missing");
    const auto& section = context.at("sections").front();
    expect(section.at("indexed_text") == body && section.at("indexed_text_omitted_bytes") == 0,
           "resident searchable body differs from source");
    expect(section.at("path") == "docs/full-body.md" && section.at("start_line") == 1 &&
               section.at("end_line") == 2, "resident section lost original range");
    expect(section.at("preview").get<std::string>().find("LateResidentNeedle") == std::string::npos,
           "late-body fixture no longer lies beyond the preview");
    expect(pack.at("completion").at("complete") == true && pack.at("evidence").size() == 1,
           "resident Pack did not share the full indexed record");
    const auto& evidence = pack.at("evidence").front();
    expect(evidence.at("matches").size() == 2 && !evidence.at("record").contains("indexed_text"),
           "Pack search text escaped display projection or lost match edges");
    expect(evidence.at("record").at("indexed_text_bytes") == body.size() &&
               pack.at("evidence").dump().size() <= 4000, "Pack body consumed the display budget");
    expect_equal(pack, render_pack(options, request, resident, 2), "resident serial/parallel body Pack");
    {
        RetrievalSession disk(options, RetrievalSessionOpenMode::verified_snapshot);
        expect_equal(context, disk.context(late), "disk/resident full body");
        expect_equal(pack, render_pack(options, request, disk, 1), "disk/resident body Pack");
    }

    const auto generated = options.out_root / kGeneratedDir;
    const auto hidden = options.out_root / "hidden-section-publication";
    fs::rename(generated, hidden);
    try {
        expect_equal(context, resident.context(late), "full body without disk artifacts");
        expect_equal(pack, render_pack(options, request, resident, 2), "body Pack without disk artifacts");
    } catch (...) {
        fs::rename(hidden, generated);
        throw;
    }
    fs::rename(hidden, generated);

    body.replace(body.size() - 9, 9, "tailomega");
    write_text(body_path, "# Full body\n" + body + "\n");
    publish_generation(options);
    const auto next_snapshot = GeneratedSnapshot::load(options);
    RetrievalSession next(next_snapshot, true);
    const auto next_pack = render_pack(options, request, next, 1);
    const auto& next_evidence = next_pack.at("evidence").front();
    expect_equal(evidence.at("record"), next_evidence.at("record"), "unchanged display projection");
    expect(evidence.at("evidence_id") != next_evidence.at("evidence_id"),
           "hidden searchable change did not change exact evidence identity");
    expect_equal(context, resident.context(late), "old full body after new publication");
    expect_cli_error([&] { (void)RetrievalSession(snapshot, true); },
                     "snapshot-generation-not-current", "old section generation admission");

    const auto& stored = snapshot->file(options.out_root / kGeneratedDir / "sections.jsonl");
    const auto row = parse_json_text(std::string(stored.substr(0, stored.find('\n'))), "section fixture");
    const auto decoded = decode_generated_section_row(row.dump());
    auto different = decoded;
    different.indexed_text += " changed";
    expect(decoded != different &&
               retrieval_unit_key(decoded, {}).record_key != retrieval_unit_key(different, {}).record_key,
           "independent section identity omitted searchable text");
    for (const auto& key : {"indexed_text", "indexed_text_omitted_bytes"}) {
        auto missing = row;
        missing.erase(key);
        expect_cli_error([&] { (void)decode_generated_section_row(missing.dump()); },
                         "index-corrupt", "missing section publication field");
    }
    for (const auto& value : {Json(-1), Json(1.5), Json(true), Json("0"), Json(2147483648LL)}) {
        auto invalid = row;
        invalid["indexed_text_omitted_bytes"] = value;
        expect_cli_error([&] { (void)decode_generated_section_row(invalid.dump()); },
                         "index-corrupt", "invalid omitted-byte count");
    }
    for (const auto& value : {Json(nullptr), Json(12), Json(std::string(kMaxSectionIndexedTextBytes + 1, 'x'))}) {
        auto invalid = row;
        invalid["indexed_text"] = value;
        expect_cli_error([&] { (void)decode_generated_section_row(invalid.dump()); },
                         "index-corrupt", "invalid section text");
    }
}

void assert_carry_forward_selection(const fs::path& parent) {
    TempDirectory temporary(parent);
    auto options = make_fixture_options(temporary.path());
    options.profile.source_extensions = {".md", ".mdc", ".json"};
    options.profile.root_files.push_back(".rules/native.mdc");
    options.profile.json_key_sections = true;
    options.profile.json_key_max_depth = 1;
    options.profile.markdown_chunk_lines = 0;
    write_text(options.root / "README.md", "# Fixture\n");
    write_text(options.root / ".rules/native.mdc",
               "---\nalwaysApply: true\n---\n# Rule\n\nOrbitConstraintNeedle stays local.\n");
    write_text(options.root / "docs/one.md", "# Overview\n\nalpha beta gamma delta epsilon.\n");
    write_text(options.root / "docs/two.md", "# Overview\n\nalpha beta gamma delta epsilon.\n");
    write_text(options.root / "docs/focused.md", "# Alpha beta gamma delta\n\nBounded evidence.\n");
    write_text(options.root / "docs/catalog.json", Json{
        {"schemaVersion", "fixture.catalog.v1"}, {"document", "catalog"},
        {"status", "active"}, {"title", "Catalog contract"}, {"summary", "Shared entry."},
        {"alpha", {"waiting", "complete"}}, {"beta", {"add", "remove"}}
    }.dump(2) + "\n");
    publish_generation(options);
    const auto snapshot = GeneratedSnapshot::load(options);
    const std::array requests{
        query(options, {"alpha", "beta", "gamma", "delta", "epsilon"}, 8, 1),
        query(options, {"docs/catalog.json", "alpha", "beta"}, 6, 2),
        query(options, {"OrbitConstraintNeedle"}, 4, 1)
    };
    RetrievalSession memory(snapshot, false);
    std::vector<Json> baseline;
    {
        RetrievalSession disk(options, RetrievalSessionOpenMode::verified_snapshot);
        for (const auto& request : requests) {
            baseline.push_back(memory.brief(request));
            expect_equal(baseline.back(), disk.brief(request), "CF3/CF4 disk/resident parity");
        }
    }
    expect(std::any_of(baseline[0].at("ranked_paths").begin(), baseline[0].at("ranked_paths").end(),
                      [](const auto& row) { return row.at("path") == "docs/focused.md"; }),
           "resident near-complete section was lost");
    expect(baseline[1].at("sections").size() == 2 && baseline[1].at("contracts").empty(),
           "resident owner-only contract displaced a matching qualifier");
    expect(baseline[2].at("sections").size() == 1 &&
               baseline[2].at("sections").front().at("path") == ".rules/native.mdc",
           "resident MDC rule lost original source ownership");
    const auto generated = options.out_root / kGeneratedDir;
    const auto hidden = options.out_root / "hidden-carry-forward";
    fs::rename(generated, hidden);
    try {
        for (std::size_t index = 0; index < requests.size(); ++index) {
            expect_equal(baseline[index], memory.brief(requests[index]),
                         "CF3/CF4 resident retrieval requires no disk artifact fallback");
        }
    } catch (...) {
        fs::rename(hidden, generated);
        throw;
    }
    fs::rename(hidden, generated);
}

int run_warm_snapshot_test(const fs::path& temporary_parent) {
    assert_lexical_word_match_equivalence();
    assert_section_publication(temporary_parent);
    assert_carry_forward_selection(temporary_parent);
    std::vector<CandidateRows> groups(2);
    for (auto& group : groups) {
        group.from_terms_index = true;
        group.term_index_coverage_complete = true;
    }
    groups[0].facts.insert(4);
    groups[0].discovered_row_count = 1;
    const auto relaxed = select_generated_group_candidates(groups, 1);
    expect(relaxed.from_terms_index && relaxed.facts == std::set<int>{4},
           "complete empty group must not invalidate a valid relaxed match");
    const auto strict = select_generated_group_candidates(groups, 2);
    expect(strict.from_terms_index && strict.discovered_row_count == 0,
           "complete strict empty intersection must remain indexed");
    groups[1].from_terms_index = false;
    groups[1].discovered_row_count = 20001;
    expect(!select_generated_group_candidates(groups, 1).from_terms_index,
           "capped group must not masquerade as an empty complete posting");
    expect(!filter_document_coverage_candidates(RuntimeOptions{}, groups, 1, {}).has_value(),
           "capped document group must reject before attempting index reads");
    groups[1].term_index_coverage_complete = false;
    QueryRequest incomplete_query;
    incomplete_query.term_groups = {{"alpha"}, {"beta"}};
    incomplete_query.prepared_term_groups = prepare_term_groups(incomplete_query.term_groups);
    const auto sparse = collect_brief_from_index_document_coverage_plan(
        RuntimeOptions{}, incomplete_query, false, relaxed_required_group_matches(incomplete_query),
        false, &groups);
    expect(sparse.planner_rejection_reason == "document-postings-incomplete" && sparse.candidate_rows == 0,
           "sparse planner must reject incomplete postings without reading partial evidence");

    TempDirectory temporary(temporary_parent);
    RuntimeOptions options = make_fixture_options(temporary.path());
    write_generation_a(options);
    publish_generation(options);

    const auto snapshot = GeneratedSnapshot::load(options);
    expect(snapshot->id().size() == 24 && snapshot->id().starts_with("fnv1a64:"),
           "snapshot id must use the fnv1a64: prefix and 16 hexadecimal digits");
    expect(snapshot->payload_bytes() > 0, "snapshot payload must own generated bytes");
    expect(snapshot->contains(options.out_root / kGeneratedDir / "terms.lex"),
           "snapshot omitted the term store");
    expect(snapshot->contains(options.out_root / kGeneratedDir / "document-rows.json"),
           "snapshot omitted the document row index");
    expect(!snapshot->file(options.out_root / kGeneratedDir / "manifest.json").empty(),
           "snapshot omitted the manifest");

    assert_disk_memory_parity(options, snapshot);
    assert_independent_cursors(options, snapshot);
    assert_snapshot_survives_missing_artifacts(options, snapshot);
    assert_load_rejections(temporary_parent);
    write_generation_b(options);
    assert_stale_source_admission(snapshot);
    assert_resident_generation_does_not_block_publish(options, snapshot);
    assert_changed_generation_admission(options, snapshot);
    return 0;
}

}  // namespace
}  // namespace shin::context

int main(int argc, char** argv) {
    try {
        if (argc > 2) {
            std::cerr << "usage: warm-snapshot-test [temporary-parent]\n";
            return EXIT_FAILURE;
        }
        const fs::path parent = argc == 2 ? fs::absolute(argv[1]) : fs::temp_directory_path();
        if (!fs::is_directory(parent)) {
            std::cerr << "temporary parent is not a directory: " << parent << '\n';
            return EXIT_FAILURE;
        }
        return shin::context::run_warm_snapshot_test(parent);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
