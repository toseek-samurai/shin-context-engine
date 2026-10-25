#include "source_discovery.h"
#include "context_internal.h"
#include "generated_io.h"
#include "generated_snapshot.h"
#include "source_cache.h"
#include "source_context.h"
#include "generation_admission.h"

#include <chrono>
#include <future>
#include <iostream>
#include <latch>
#ifdef _WIN32
#include "windows_private_acl.h"
#endif

namespace shin::context {
namespace {
void expect(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
template<typename Callback> void rejected(Callback callback, const std::string& code = "") {
    try { callback(); } catch (const CliError& error) {
        expect(code.empty() || error.code() == code, "unexpected refusal code");
        return;
    }
    throw std::runtime_error("expected source discovery refusal");
}
class Fixture {
public:
    explicit Fixture(const fs::path& parent) : base(parent / ("source-discovery-" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()))) {
        options.root = fs::absolute(base / "root");
        options.out_root = fs::absolute(base / "index");
        options.profile.id = "source-discovery-fixture";
        options.profile.config_hash = "source-discovery-fixture-v1";
        options.profile.root_files = {"README.md"};
        options.profile.source_fact_roots = {"src"};
        options.profile.source_fact_extensions = {".cpp", ".sql", ".rsx"};
        options.profile.source_fact_extractors = {"language"};
        options.profile.source_fact_max_file_bytes = 2 * 1024 * 1024;
        write("README.md", "# Authentication boundary reference\n\nneedleExactMarker document only.\n");
    }
    ~Fixture() { std::error_code error; fs::remove_all(base, error); }
    void write(const std::string& path, const std::string& text) {
        fs::create_directories((options.root / path).parent_path());
        std::ofstream stream(options.root / path, std::ios::binary);
        stream << text;
        stream.close();
        expect(!stream.fail(), "write fixture");
    }
    void publish() {
        const auto generated = generate_outputs(options.profile, options.root);
        shin::generated_io::IndexLock lock(options.out_root, shin::generated_io::LockMode::exclusive,
                                           shin::generated_io::LockTarget::writer);
        shin::generated_io::publish_snapshot(options.out_root, kGeneratedDir, generated.files);
    }
    fs::path base;
    RuntimeOptions options;
};

void check(const fs::path& parent) {
    expect(source_search_projection("SqlRowWindow HTTPServer foo_bar a2B unchanged") ==
           "SqlRowWindow HTTPServer foo_bar a2B unchanged\nSql Row Window HTTP Server a2 B", "identifier projection");
    const std::string unicode = "\xd0\xba\xd0\xbe\xd0\xb4";
    expect(source_search_projection(unicode + " SqlRowWindow") == unicode + " SqlRowWindow\nSql Row Window",
           "raw non-ASCII text preserved");
    Fixture f(parent);
    f.write("src/auth.cpp", "// authentication boundary needleExactMarker\nint authorize() { return 1; }\n");
    f.write("src/query.sql", "SELECT uniquePayloadMarker FROM source_table;\n");
    f.write("src/empty.rsx", "");
    const std::string secret = "sk-" + std::string(64, 'Q');
    f.write("src/secret.cpp", "// privateMarker token = \"" + secret + "\";\n");
    f.publish();
    auto result = discover_source_candidates(f.options, {"needleExactMarker", "uniquePayloadMarker", "nonexistentZZUnique", "\" OR * NOT"});
    expect(result.at("ok") == true && result.at("redacted_files") == 1 && result.at("files") == 4, "source inventory and redaction");
    expect(result.at("results")[0].at("candidates")[0].at("path") == "src/auth.cpp", "source rather than document");
    expect(result.at("results")[1].at("candidates")[0].at("path") == "src/query.sql", "source without extracted declaration");
    expect(result.at("results")[2].at("candidates").empty(), "no lexical evidence means empty");
    expect(result.dump().find(secret) == std::string::npos, "no sensitive body");
    expect(result.at("results")[0].at("answer_complete") == false, "ranking is not answer proof");
    rejected([&] { discover_source_candidates(f.options, {}); });
    rejected([&] { discover_source_candidates(f.options, {std::string(4097, 'x')}); });
    rejected([&] { discover_source_candidates(f.options, {std::string("one\0two", 7)}); });
    std::string many_terms;
    for (int i = 0; i < 65; ++i) many_terms += "word" + std::to_string(i) + " ";
    rejected([&] { discover_source_candidates(f.options, {many_terms}); });
    prepare_freshness_cache(f.options);
    const auto modified = fs::last_write_time(f.options.root / "src/auth.cpp");
    const auto original = read_file(f.options.root / "src/auth.cpp");
    auto different = original;
    different.back() = ' ';
    f.write("src/auth.cpp", different);
    fs::last_write_time(f.options.root / "src/auth.cpp", modified);
    rejected([&] { discover_source_candidates(f.options, {"needleExactMarker"}); });
    f.write("src/auth.cpp", original);
    f.write("src/extra.sql", "SELECT added_file;\n");
    rejected([&] { discover_source_candidates(f.options, {"needleExactMarker"}); });
    fs::remove(f.options.root / "src/extra.sql");
    f.write("src/auth.cpp", "int changed() { return 0; }\n");
    rejected([&] { discover_source_candidates(f.options, {"needleExactMarker"}); });
    f.publish();
    fs::remove(f.options.root / "src/auth.cpp");
    fs::create_symlink("query.sql", f.options.root / "src/auth.cpp");
    rejected([&] { discover_source_candidates(f.options, {"needleExactMarker"}); });

    Fixture capped(parent);
    std::string repeated;
    for (int i = 0; i < 200; ++i) repeated += "SELECT boundedNeedle FROM independent_values;\n";
    for (int i = 0; i < 6; ++i)
        capped.write("src/unit" + std::to_string(i) + ".sql", repeated);
    capped.publish();
    result = discover_source_candidates(capped.options, {"boundedNeedle"});
    expect(result.at("results")[0].at("candidates").size() == 8, "candidate count cap");
    std::map<std::string, int> counts;
    for (const auto& row : result.at("results")[0].at("candidates"))
        expect(++counts[row.at("path").get<std::string>()] <= 2, "per file cap");
    capped.write("src/huge.cpp", std::string(1024 * 1024 + 1, ' '));
    capped.publish();
    rejected([&] { discover_source_candidates(capped.options, {"boundedNeedle"}); });
    capped.options.profile.source_fact_max_files = 1;
    capped.publish();
    rejected([&] { discover_source_candidates(capped.options, {"boundedNeedle"}); });

    Fixture identifiers(parent);
    identifiers.write("src/window.sql", "SELECT hasOperatorRowWindow, SqlPaginationMode FROM x;\n");
    identifiers.publish();
    const auto split = discover_source_candidates(identifiers.options, {"row window", "pagination", "SqlPaginationMode", "absentNeedle"});
    for (std::size_t i = 0; i < 3; ++i)
        expect(split.at("results")[i].at("candidates")[0].at("path") == "src/window.sql", "natural and exact identifier recall");
    expect(split.at("results")[3].at("candidates").empty(), "no invented lexical evidence");
}

void natural_language_checks(const fs::path& parent) {
    Fixture f(parent);
    f.write("src/retry.cpp", "// Retry failed requests after a network timeout.\nint retryRequest() { return 1; }\n");
    f.write("src/permissions.cpp", "// Validate user permissions before access to the database.\nint validatePermission() { return 2; }\n");
    f.write("src/russian.cpp", "// Проверка целостности архива\nint inspectArchive() { return 3; }\n");
    f.write("src/noise.sql", "SELECT whereClause, howValue, doesFlag, theValue, whenValue FROM words;\n");
    f.publish();
    const auto result = discover_source_candidates(f.options, {
        "Where does retrying a failed network request happen?",
        "Где повторяется запрос после таймаута сети?",
        "Где проверяются права пользователя перед доступом к базе?",
        "ПРОВЕРКА ЦЕЛОСТНОСТИ АРХИВА", "validatePermission",
        "секретарьквазипанголин", "Where does quuxPangolinNonexistent happen?"});
    const std::vector<std::string> expected = {"src/retry.cpp", "src/retry.cpp", "src/permissions.cpp",
                                              "src/russian.cpp", "src/permissions.cpp"};
    for (std::size_t i = 0; i < expected.size(); ++i) {
        const auto& candidates = result.at("results")[i].at("candidates");
        expect(!candidates.empty() && candidates[0].at("path") == expected[i],
               ("natural language owner: " + std::to_string(i)).c_str());
    }
    for (std::size_t i : {5, 6})
        expect(result.at("results")[i].at("candidates").empty(), "no stop-word-only or invented language evidence");
    const auto forms = discover_source_candidates(f.options, {"validate permission", "validating validated permission permissions"});
    expect(forms.at("results")[0].at("candidates") == forms.at("results")[1].at("candidates"),
           "equivalent inflections must not multiply BM25 weight");

    Fixture breadth(parent);
    std::string repeated;
    for (int i = 0; i < 200; ++i) repeated += "SELECT retry, request, network, timeout FROM values_table;\n";
    for (int i = 0; i < 8; ++i) breadth.write("src/part" + std::to_string(i) + ".sql", repeated);
    breadth.publish();
    auto diverse = discover_source_candidates(breadth.options, {"retry request network timeout", "retry", "состояние"});
    const auto& broad = diverse.at("results")[0];
    expect(broad.at("distinct_paths_first") == true && broad.at("candidates").size() == 8, "broad query keeps the span cap");
    std::set<std::string> paths;
    for (const auto& candidate : broad.at("candidates")) paths.insert(candidate.at("path"));
    expect(paths.size() == 8, "different files precede duplicate spans for broad discovery");
    expect(diverse.at("results")[1].at("distinct_paths_first") == false &&
           diverse.at("results")[1].at("candidates")[0].at("path") == diverse.at("results")[1].at("candidates")[1].at("path"),
           "short symbol queries preserve BM25 span order");
    expect(diverse.at("results")[2].at("distinct_paths_first") == false, "translation hints cannot trigger broad-query diversity");
    for (int i = 2; i < 8; ++i) fs::remove(breadth.options.root / ("src/part" + std::to_string(i) + ".sql"));
    breadth.publish();
    diverse = discover_source_candidates(breadth.options, {"retry request network timeout"});
    const auto& filled = diverse.at("results")[0].at("candidates");
    expect(filled.size() == 4 && filled[0].at("path") != filled[1].at("path") && filled[0].at("path") == filled[2].at("path"),
           "second spans fill spare capacity without exceeding two per file");
}

void resident_checks(const fs::path& parent) {
    Fixture f(parent);
    f.write("src/live.sql", "SELECT stableResidentNeedle;\n");
    f.publish();
    SourceDiscoveryIndex index(f.options);
    const auto cold = discover_source_candidates(f.options, {"stableResidentNeedle"});
    const auto first = index.query(f.options, {"stableResidentNeedle"});
    const auto second = index.query(f.options, {"stableResidentNeedle"});
    expect(first.at("results")[0].at("candidates") == cold.at("results")[0].at("candidates") &&
           first.at("results")[0].at("candidates") == second.at("results")[0].at("candidates"), "cold and reused parity");
    expect(first.at("reuse").at("query_sequence") == 1 && second.at("reuse").at("query_sequence") == 2 &&
           second.at("reuse").at("reused") == true && second.at("preparation_ms") == 0 &&
           second.at("reuse").at("index_builds") == 1, "no hidden rebuild on reuse");
    auto wrong = f.options;
    wrong.root = f.base / "another-project";
    rejected([&] { index.query(wrong, {"stableResidentNeedle"}); }, "source-discovery-binding-mismatch");
    wrong = f.options;
    wrong.out_root = f.base / "another-index";
    rejected([&] { index.query(wrong, {"stableResidentNeedle"}); }, "source-discovery-binding-mismatch");
    wrong = f.options;
    wrong.profile.config_hash = "changed-policy";
    rejected([&] { index.query(wrong, {"stableResidentNeedle"}); }, "source-discovery-binding-mismatch");
    rejected([&] { index.query(f.options, {}); }, "source-discovery-query-limit");
    expect(index.query(f.options, {"stableResidentNeedle"}).at("reuse").at("query_sequence") == 3,
           "bad requests do not invalidate good index");

    std::latch start(8);
    std::vector<std::future<bool>> callers;
    callers.reserve(8);
    for (int i = 0; i < 8; ++i) callers.push_back(std::async(std::launch::async, [&] {
        start.arrive_and_wait();
        try {
            const auto result = index.query(f.options, {"stableResidentNeedle"});
            expect(result.at("results")[0].at("candidates") == first.at("results")[0].at("candidates"), "concurrent parity");
            return true;
        } catch (const CliError& error) {
            expect(error.code() == "source-discovery-busy", "concurrent calls either succeed or fail busy");
            return false;
        }
    }));
    int successes = 0;
    for (auto& caller : callers) successes += caller.get();
    expect(successes > 0, "at least one concurrent request completes");

    f.write("src/live.sql", "SELECT replacedResidentNeedle;\n");
    rejected([&] { index.query(f.options, {"stableResidentNeedle"}); });
    f.write("src/live.sql", "SELECT stableResidentNeedle;\n");
    rejected([&] { index.query(f.options, {"stableResidentNeedle"}); }, "source-discovery-invalidated");
    SourceDiscoveryIndex restored(f.options);
    expect(restored.query(f.options, {"stableResidentNeedle"}).at("ok") == true, "explicit reprepare restores service");
    f.write("src/live.sql", "SELECT newGenerationNeedle;\n");
    f.publish(); // Must not wait on an idle resident object's publication lock.
    rejected([&] { restored.query(f.options, {"stableResidentNeedle"}); }, "source-discovery-snapshot-changed");
    SourceDiscoveryIndex updated(f.options);
    expect(updated.query(f.options, {"newGenerationNeedle"}).at("results")[0].at("candidates").size() == 1,
           "new generation is selected only by explicit preparation");

    Fixture policy(parent);
    policy.write("src/live.sql", "SELECT profileDriftNeedle;\n");
    const auto config = policy.base / "profile.json";
    { std::ofstream stream(config); stream << "{\"policy\":1}"; }
    policy.options.profile.config_source = config.generic_string();
    policy.options.profile.config_hash = fnv1a64(read_file(config));
    policy.publish();
    SourceDiscoveryIndex policy_index(policy.options);
    { std::ofstream stream(config); stream << "{\"policy\":2}"; }
    rejected([&] { policy_index.query(policy.options, {"profileDriftNeedle"}); });
    { std::ofstream stream(config); stream << "{\"policy\":1}"; }
    rejected([&] { policy_index.query(policy.options, {"profileDriftNeedle"}); }, "source-discovery-invalidated");
}

void snapshot_checks(const fs::path& parent) {
    Fixture f(parent);
    f.options.profile.source_fact_extensions = {".ts"};
    const std::string owner = "import { checked } from './payload';\nexport const selectedSnapshotNeedle = checked();\n";
    f.write("src/owner.ts", owner);
    f.write("src/payload.ts", "export const checked = () => 42;\n");
    f.write("src/unrelated.ts", "export const unrelated = 1;\n");
    f.publish();
    const auto snapshot = GeneratedSnapshot::load(f.options);
    auto binding = f.options;
    binding.generated_snapshot = snapshot;
    const auto cache = fs::absolute(f.base / "snapshot-cache");
    {
        SourceDiscoveryIndex disk(f.options);
        rejected([&] { disk.query_at_snapshot(binding, {"selectedSnapshotNeedle"}); },
                 "source-discovery-binding-mismatch");
        rejected([&] { disk.context_at_snapshot(binding, "selectedSnapshotNeedle"); },
                 "source-discovery-binding-mismatch");
    }
    Json baseline;
    {
        SourceDiscoveryIndex cold(binding, cache);
        baseline = cold.query_at_snapshot(binding, {"selectedSnapshotNeedle"});
        expect(baseline.at("cache").at("status") == "created", "snapshot source cache created");
        auto other = binding;
        other.generated_snapshot = GeneratedSnapshot::load(f.options);
        rejected([&] { cold.query_at_snapshot(other, {"selectedSnapshotNeedle"}); },
                 "source-discovery-binding-mismatch");
        rejected([&] { cold.context_at_snapshot(other, "selectedSnapshotNeedle"); },
                 "source-discovery-binding-mismatch");
        rejected([&] { cold.query_at_snapshot(f.options, {"selectedSnapshotNeedle"}); },
                 "source-discovery-binding-mismatch");
        rejected([&] { cold.context_at_snapshot(f.options, "selectedSnapshotNeedle"); },
                 "source-discovery-binding-mismatch");
        other = binding;
        other.root = fs::absolute(f.base / "other-project");
        rejected([&] { SourceDiscoveryIndex wrong(other); }, "source-discovery-binding-mismatch");
        f.write("src/owner.ts", owner + "// outside selected chunk\n");
        rejected([&] { cold.query_at_snapshot(binding, {"selectedSnapshotNeedle"}); }, "source-read-hash-mismatch");
        f.write("src/owner.ts", owner);
        expect(cold.query(binding, {"selectedSnapshotNeedle"}).at("index_freshness").at("current") == true,
               "regular snapshot-backed query still assesses global currentness");
    }
    SourceDiscoveryIndex warm(binding, cache);
    const auto restored = warm.query_at_snapshot(binding, {"selectedSnapshotNeedle"});
    expect(restored.at("cache").at("status") == "restored" && restored.at("reuse").at("index_builds") == 0,
           "snapshot source cache restored without rebuilding");
    f.write("src/unrelated.ts", "export const replacedUnrelated = 2;\n");
    f.write("src/added.ts", "export const added = 3;\n");
    const auto count = generation_execution_count();
    const auto query = warm.query_at_snapshot(binding, {"selectedSnapshotNeedle"});
    expect(query.at("results")[0].at("candidates") == baseline.at("results")[0].at("candidates") &&
           query.at("reuse").at("reused") == true && query.at("preparation_ms") == 0,
           "snapshot query reuses resident source cache despite unrelated changes");
    const auto& freshness = query.at("index_freshness");
    expect(freshness.at("status") == "not-assessed" && freshness.at("current") == false &&
           freshness.at("fully_verified") == false && query.at("snapshot_id") == snapshot->id(),
           "snapshot discovery does not claim global freshness");
    expect(generation_execution_count() == count, "snapshot query does not regenerate the corpus");
    rejected([&] { SourceDiscoveryIndex stale(binding, cache); });
    const auto context = warm.context_at_snapshot(binding, "selectedSnapshotNeedle");
    expect(context.at("context").at("bodies").size() == 2 &&
           context.at("context").at("bodies")[0].at("text") == owner &&
           context.at("index_freshness").at("status") == "not-assessed",
           "snapshot context materializes selected and declared related sources only");
    f.write("src/payload.ts", "export const checked = () => 43;\n");
    expect(warm.query_at_snapshot(binding, {"selectedSnapshotNeedle"}).at("ok") == true,
           "metadata query does not traverse unreturned related bodies");
    rejected([&] { warm.context_at_snapshot(binding, "selectedSnapshotNeedle"); }, "source-read-hash-mismatch");
    f.write("src/payload.ts", "export const checked = () => 42;\n");
    f.write("src/owner.ts", owner + "// stale selected source\n");
    rejected([&] { warm.query_at_snapshot(binding, {"selectedSnapshotNeedle"}); }, "source-read-hash-mismatch");
    rejected([&] { warm.context_at_snapshot(binding, "selectedSnapshotNeedle"); }, "source-read-hash-mismatch");
    f.write("src/owner.ts", owner);
    rejected([&] { warm.query(binding, {"selectedSnapshotNeedle"}); });
    rejected([&] { warm.query_at_snapshot(binding, {"selectedSnapshotNeedle"}); }, "source-discovery-invalidated");
    rejected([&] { warm.context(binding, "selectedSnapshotNeedle"); }, "source-discovery-invalidated");
}

void context_checks(const fs::path& parent) {
    Fixture f(parent);
    f.options.profile.source_fact_extensions = {".ts", ".tsx"};
    f.write("src/owner.ts", "import { checked } from './payload';\nexport const navigationNeedle = checked();\n");
    f.write("src/payload.ts", "export const checked = () => 42;\n");
    f.publish();
    SourceDiscoveryIndex index(f.options);
    const auto found = index.query(f.options, {"navigationNeedle"});
    const auto result = index.context(f.options, "navigationNeedle");
    expect(result.at("schema_version") == "shin.source-context.v1", "context schema");
    expect(result.at("results")[0].at("candidates") == found.at("results")[0].at("candidates"), "collection never reranks");
    const auto& context = result.at("context");
    expect(context.at("answer_complete") == false && context.at("collection_complete") == true, "bounded collection not answer proof");
    expect(context.at("bodies").size() == 2 && context.at("relations").size() == 1, "one-hop source delivery");
    expect(context.at("bodies")[0].at("text") == read_file(f.options.root / "src/owner.ts"), "selected raw bytes");
    expect(context.at("bodies")[1].at("text") == "export const checked = () => 42;\n" &&
           context.at("bodies")[1].at("role") == "related", "linked exact body");
    expect(index.context(f.options, "opaqueAbsentUniqueMarker").at("context").at("bodies").empty(), "empty query does not collect arbitrary context");
    expect(index.context(f.options, "opaqueAbsentUniqueMarker").at("context").at("collection_complete") == false, "empty is not complete evidence");
    const auto cache = fs::absolute(f.base / "cache");
    { SourceDiscoveryIndex cold(f.options, cache); (void)cold.context(f.options, "navigationNeedle"); }
    { SourceDiscoveryIndex warm(f.options, cache);
      expect(warm.context(f.options, "navigationNeedle").at("context") == context, "restored context parity"); }
    f.write("src/payload.ts", "export const checked = () => 43;\n");
    rejected([&] { index.context(f.options, "navigationNeedle"); });

    Fixture withheld(parent);
    withheld.options.profile.source_fact_extensions = {".ts", ".tsx"};
    withheld.write("src/owner.ts", "import { secret } from './secret';\nimport { large } from './large';\n"
                                   "import { route } from './dual';\nexport const guardedContextNeedle = 1;\n");
    const std::string secret = "sk-" + std::string(64, 'Q');
    withheld.write("src/secret.ts", "export const secret = \"" + secret + "\"; // gitleaks:allow\n");
    withheld.write("src/large.ts", "export const large = 1;\n/*" + std::string(5000, 'a') + "*/\n");
    withheld.write("src/dual.ts", "export const route = 1;\n");
    withheld.write("src/dual.tsx", "export const route = 2;\n");
    withheld.publish();
    SourceDiscoveryIndex guarded(withheld.options);
    const auto incomplete = guarded.context(withheld.options, "guardedContextNeedle");
    expect(incomplete.dump().find(secret) == std::string::npos, "linked secret suppression cannot opt out");
    expect(incomplete.at("context").at("bodies").size() == 1, "no secret large or ambiguous linked body");
    expect(incomplete.at("context").at("collection_complete") == false, "omissions are not hidden");
    std::set<std::string> reasons;
    for (const auto& row : incomplete.at("context").at("omissions")) reasons.insert(row.at("reason"));
    expect(reasons == std::set<std::string>{"redacted", "related-file-too-large"}, "specific omission reasons");
    for (const auto& edge : incomplete.at("context").at("relations"))
        expect(edge.at("target_path") != "src/dual.ts" && edge.at("target_path") != "src/dual.tsx", "no invented ambiguous link");

    Fixture late(parent);
    late.options.profile.source_fact_extensions = {".ts"};
    std::string owner;
    for (int i = 0; i < 24; ++i) owner += "import { earlier" + std::to_string(i) + " } from './early';\n";
    const auto begin = owner.size();
    owner += "import { producer } from './producer';\n";
    late.write("src/owner.ts", owner);
    late.write("src/early.ts", "export const early = 0;\n");
    const std::string producer = "export const producer = 1;\nexport const requiredTail = 2;\n";
    late.write("src/producer.ts", producer);
    late.publish();
    const auto candidate = [](const std::string& path, const std::string& text, std::size_t first, std::size_t last) {
        return Json{{"path", path}, {"byte_start", first}, {"byte_end", last},
            {"source_content_hash", fnv1a64(text)}, {"span_hash", fnv1a64(text.substr(first, last - first))},
            {"start_line", 1 + std::count(text.begin(), text.begin() + static_cast<std::ptrdiff_t>(first), '\n')},
            {"end_line", 1 + std::count(text.begin(), text.begin() + static_cast<std::ptrdiff_t>(last - 1), '\n')}};
    };
    RetrievalSession session(late.options, RetrievalSessionOpenMode::verified_snapshot);
    const auto late_context = collect_source_context(late.options, session.freshness().snapshot_id,
        Json::array({candidate("src/owner.ts", owner, begin, owner.size()),
                     candidate("src/producer.ts", producer, 0, producer.find('\n') + 1)}),
        {{"src/owner.ts", fnv1a64(owner)}, {"src/producer.ts", fnv1a64(producer)}});
    expect(late_context.at("relations").size() == 1 &&
           late_context.at("relations")[0].at("target_path") == "src/producer.ts", "filter range before link cap");
    expect(late_context.at("bodies").size() == 3 && late_context.at("bodies")[2].at("text") == producer,
           "selected fragment does not suppress complete dependency");
    expect(late_context.at("bodies")[2].at("source_line") == 25 &&
           late_context.at("bodies")[2].at("source_path") == "src/owner.ts", "dependency retains consumer coordinates");
    expect(late_context.at("collection_complete") == true, "unselected early links do not consume the task budget");

    Fixture budget(parent);
    budget.options.profile.source_fact_extensions = {".ts"};
    Json crowded = Json::array();
    std::map<std::string, std::string> budget_sources;
    for (int i = 0; i < 8; ++i) {
        const auto path = "src/unit" + std::to_string(i) + ".ts";
        const auto content = std::string(i == 0 ? "import './dependency';\n" : "") + "//" + std::string(970, '\\') + "\n";
        budget.write(path, content);
        budget_sources[path] = fnv1a64(content);
        crowded.push_back(candidate(path, content, 0, content.size()));
    }
    const auto dependency = "export const dependency = '" + std::string(3000, 'x') + "';\n";
    budget.write("src/dependency.ts", dependency);
    budget_sources["src/dependency.ts"] = fnv1a64(dependency);
    budget.publish();
    RetrievalSession budget_session(budget.options, RetrievalSessionOpenMode::verified_snapshot);
    const auto limited = collect_source_context(budget.options, budget_session.freshness().snapshot_id, crowded, budget_sources);
    const auto missing = std::find_if(limited.at("omissions").begin(), limited.at("omissions").end(), [](const Json& row) {
        return row.at("path") == "src/dependency.ts";
    });
    expect(missing != limited.at("omissions").end() && missing->at("reason") == "serialized-body-budget",
           "independent TypeScript fixture reproduces body budget omission");
    expect(missing->at("start_line") == 1 && missing->at("end_line") == 1 && missing->contains("target_symbol") &&
           !missing->contains("text"), "omission keeps target address without rejected body");

    Fixture declarations(parent);
    declarations.options.profile.source_fact_roots = {"apps"};
    declarations.options.profile.source_fact_extensions = {".rsx"};
    declarations.options.profile.source_fact_extractors = {"retool"};
    const std::string file = "apps/admin/Example/functions.rsx";
    std::string definitions = "<GlobalFunctions>\n<State id=\"Consumer\" value=\"{{ First.data + Second.data }}\" />\n";
    const auto selected_end = definitions.size();
    definitions += "<!--" + std::string(5000, 'p') + "-->\n";
    const std::string first = "<RESTQuery id=\"First\"\n query=\"/first\" body=\"{{ requestPayload }}\" />\n";
    const std::string second = "<RESTQuery id=\"Second\" query=\"/second\" />\n";
    definitions += first + second + "</GlobalFunctions>\n";
    declarations.write(file, definitions);
    declarations.publish();
    RetrievalSession declaration_session(declarations.options, RetrievalSessionOpenMode::verified_snapshot);
    const auto declared = collect_source_context(declarations.options, declaration_session.freshness().snapshot_id,
        Json::array({candidate(file, definitions, 0, selected_end)}), {{file, fnv1a64(definitions)}});
    expect(declared.at("bodies").size() == 3 && declared.at("bodies")[1].at("text") == first &&
           declared.at("bodies")[2].at("text") == second, "distinct same-file targets survive path deduplication");
    expect(declared.at("bodies")[1].at("range_origin") == "rsx-opening-tag" &&
           declared.at("bodies")[1].at("byte_start").get<std::size_t>() > 4096 &&
           declared.at("collection_complete") == true, "bounded declaration from an oversized file");
    const Json legacy = {{"path", file}, {"line", 2}, {"kind", "source-reference"}, {"target", "First"},
        {"resolved_path", file}, {"status", "resolved"}, {"text", "First"}};
    expect(parse_link_row(legacy.dump()).target_start_line == 0, "old graph rows have no invented target range");
    auto valid = legacy;
    valid.update({{"target_start_line", 4}, {"target_end_line", 5}, {"target_range_origin", "rsx-opening-tag"}});
    expect(parse_link_row(valid.dump()).target_end_line == 5, "target range round trip");
    for (const auto& invalid : Json::array({Json{{"target_end_line", 0}}, Json{{"target_start_line", "4"}},
        Json{{"target_start_line", nullptr}}, Json{{"target_range_origin", "guessed"}}, Json{{"status", "ambiguous"}}})) {
        auto corrupt = valid;
        corrupt.update(invalid);
        rejected([&] { (void)parse_link_row(corrupt.dump()); }, "index-corrupt");
    }

    Fixture java(parent);
    java.options.profile.source_fact_extensions = {".java"};
    const std::string consumer = "import types.Needed;\nclass Consumer {}\n";
    const std::string needed = "public class Needed { public int value() { return 7; } }\n";
    const std::string types = "package types;\n" + needed + "/*" + std::string(5000, 'p') + "*/\n";
    java.write("src/Consumer.java", consumer);
    java.write("src/Needed.java", types);
    java.publish();
    RetrievalSession java_session(java.options, RetrievalSessionOpenMode::verified_snapshot);
    const auto java_context = collect_source_context(java.options, java_session.freshness().snapshot_id,
        Json::array({candidate("src/Consumer.java", consumer, 0, consumer.size())}),
        {{"src/Consumer.java", fnv1a64(consumer)}, {"src/Needed.java", fnv1a64(types)}});
    expect(java_context.at("bodies").size() == 2 && java_context.at("bodies")[1].at("text") == needed &&
           java_context.at("bodies")[1].at("range_origin") == "syntax-declaration", "same collector reads Java AST declaration ranges");
}

void disk_checks(const fs::path& parent) {
    Fixture f(parent);
    f.write("src/live.sql", "SELECT persistentNeedle;\n");
    f.publish();
    const auto cache = fs::absolute(f.base / "cache");
    const auto baseline = discover_source_candidates(f.options, {"persistentNeedle"});
    {
        SourceDiscoveryIndex first(f.options, cache);
        const auto result = first.query(f.options, {"persistentNeedle"});
        expect(result.at("cache").at("status") == "created", "first cache creation");
        expect(result.at("results")[0].at("candidates") == baseline.at("results")[0].at("candidates"), "disk cold parity");
    }
    fs::path database;
    for (const auto& file : fs::directory_iterator(cache))
        if (file.path().extension() == ".sqlite") database = file.path();
    expect(!database.empty(), "SQLite artifact exists");
    const auto before = read_file(database);
    expect(before.starts_with("SQLite format 3"), "native SQLite artifact");
    {
        SourceDiscoveryIndex reopened(f.options, cache);
        const auto result = reopened.query(f.options, {"persistentNeedle"});
        expect(result.at("cache").at("status") == "restored", "restart restores index");
        expect(result.at("reuse").at("index_builds") == 0, "restoration does not claim a source rebuild");
        expect(result.at("results")[0].at("candidates") == baseline.at("results")[0].at("candidates"), "restored parity");
        expect(read_file(database) == before, "queries never persist query text");
    }
    {
        SourceCache old_format(cache, database.stem().string());
        const auto saved = old_format.load();
        if (!saved) throw std::runtime_error("expected cache artifact");
        auto bytes = *saved;
        const auto format = bytes.find("shin.source-cache.v3");
        expect(format != std::string::npos, "cache format is bound");
        bytes.replace(format, std::string("shin.source-cache.v3").size(), "shin.source-cache.v2");
        old_format.save(bytes);
    }
    {
        SourceDiscoveryIndex upgraded(f.options, cache);
        expect(upgraded.query(f.options, {"persistentNeedle"}).at("cache").at("status") == "rebuilt",
               "old projection cache is never reused");
    }
    { std::ofstream corrupt(database, std::ios::trunc); corrupt << "interrupted artifact"; }
    {
        SourceDiscoveryIndex repaired(f.options, cache);
        expect(repaired.query(f.options, {"persistentNeedle"}).at("cache").at("status") == "rebuilt", "corruption rebuilds");
    }
    f.write("src/live.sql", "SELECT freshGenerationNeedle;\n");
    rejected([&] { SourceDiscoveryIndex stale(f.options, cache); });
    f.publish();
    {
        SourceDiscoveryIndex refreshed(f.options, cache);
        const auto result = refreshed.query(f.options, {"freshGenerationNeedle"});
        expect(result.at("cache").at("status") == "rebuilt" &&
                result.at("results")[0].at("candidates").size() == 1, "generation replacement");
    }
    expect(std::count_if(fs::directory_iterator(cache), fs::directory_iterator{}, [](const auto& entry) {
        return entry.path().extension() == ".sqlite";
    }) == 1, "one generation per binding");
#ifdef _WIN32
    fixture_private_acl(database, true);
#else
    fs::permissions(database, fs::perms::group_read, fs::perm_options::add);
#endif
    rejected([&] { SourceDiscoveryIndex unsafe(f.options, cache); }, "source-cache-unsafe-file");
#ifdef _WIN32
    fixture_private_acl(database);
#else
    fs::permissions(database, fs::perms::group_read, fs::perm_options::remove);
#endif
    const auto displaced = database.string() + ".original";
    fs::rename(database, displaced);
    fs::create_symlink(displaced, database);
    rejected([&] { SourceDiscoveryIndex unsafe(f.options, cache); });
    fs::remove(database);
    fs::create_hard_link(displaced, database);
    rejected([&] { SourceDiscoveryIndex unsafe(f.options, cache); }, "source-cache-unsafe-file");
    fs::remove(database);
    fs::rename(displaced, database);
    const std::string key = database.stem().string();
    {
        SourceCache held(cache, key);
        rejected([&] { SourceCache collision(cache, key); }, "source-cache-busy");
    }
    const auto pending = cache / (key + ".pending");
    { std::ofstream partial(pending); partial << "partial"; }
#ifdef _WIN32
    fixture_private_acl(pending);
#else
    fs::permissions(pending, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace);
#endif
    {
        SourceDiscoveryIndex recovered(f.options, cache);
        expect(!fs::exists(pending), "interrupted pending reclaimed");
        expect(recovered.query(f.options, {"freshGenerationNeedle"}).at("cache").at("status") == "restored", "old publication survives interruption");
    }
#ifdef _WIN32
    fixture_private_acl(cache, true);
#else
    fs::permissions(cache, fs::perms::group_read, fs::perm_options::add);
#endif
    rejected([&] { SourceDiscoveryIndex unsafe(f.options, cache); }, "source-cache-invalid-directory");
}
void published_cache_transfer_checks(const fs::path& parent) {
    const auto configure_admission_profile = [](Fixture& fixture) {
        fixture.options.profile_dir = fs::absolute(fixture.base / "profiles");
        fs::create_directories(fixture.options.profile_dir);
        const auto profile_path = fixture.options.profile_dir / "source-discovery-fixture.json";
        const Json profile = {{"schemaVersion", "shin.profile.v1"}, {"document", "fixture"},
            {"status", "active"}, {"summary", "source discovery admission fixture"},
            {"id", "source-discovery-fixture"}, {"sourceLabel", "source discovery fixture"},
            {"rootFiles", {"README.md"}}, {"sourceFactRoots", {"src"}},
            {"sourceFactExtensions", {".sql"}}, {"sourceFactExtractors", {"language"}}};
        std::ofstream stream(profile_path);
        stream << profile.dump();
        stream.close();
        expect(!stream.fail(), "write admission profile");
        fixture.options.profile = make_profile("source-discovery-fixture", fixture.options.profile_dir);
    };

    Fixture f(parent);
    configure_admission_profile(f);
    const std::string content = "SELECT transferredPublicationNeedle;\n";
    f.write("src/live.sql", content);
    f.publish();

    auto candidate = f.options;
    candidate.out_root = fs::absolute(f.base / "candidate-index");
    const auto generated = generate_outputs(candidate.profile, candidate.root);
    shin::generated_io::publish_snapshot(candidate.out_root, kGeneratedDir, generated.files);
    candidate.generation_admission = GenerationAdmission::reuse(candidate, generation_fingerprint(candidate));
    expect(bool(candidate.generation_admission), "candidate generation admission");
    auto published = f.options;
    published.generation_admission = candidate.generation_admission->published_copy(candidate, published);

    const auto cache = fs::absolute(f.base / "published-cache");
    SourceDiscoveryIndex prepared(candidate);
    const auto expected = prepared.query(candidate, {"transferredPublicationNeedle"});
    const std::string private_query = "neverPersistThisPrivateQuery731";
    (void)prepared.query(candidate, {private_query});
    const auto before = prepared.preparation_metrics();
    expect(before.at("index_builds") == 1 && before.at("files") == 1 &&
           before.at("source_bytes") == content.size() && before.at("chunks") > 0,
           "preparation metrics describe the actual source build");

    auto wrong_root = published;
    wrong_root.root = fs::absolute(f.base / "another-root");
    rejected([&] { prepared.persist_published_cache(wrong_root, cache); }, "source-discovery-binding-mismatch");
    auto wrong_profile = published;
    wrong_profile.profile.id += "-other";
    rejected([&] { prepared.persist_published_cache(wrong_profile, cache); }, "source-discovery-binding-mismatch");
    auto wrong_snapshot = published;
    wrong_snapshot.out_root = fs::absolute(f.base / "other-generation");
    wrong_snapshot.profile.default_topic = "different-fixture-topic";
    const auto other_generated = generate_outputs(wrong_snapshot.profile, wrong_snapshot.root);
    shin::generated_io::publish_snapshot(wrong_snapshot.out_root, kGeneratedDir, other_generated.files);
    wrong_snapshot.generation_admission = GenerationAdmission::reuse(
        wrong_snapshot, generation_fingerprint(wrong_snapshot));
    rejected([&] { prepared.persist_published_cache(wrong_snapshot, cache); }, "source-discovery-snapshot-changed");

    {
        auto candidate_lock = lock_index_for_read(candidate.out_root);
        auto published_lock = lock_index_for_read(published.out_root);
        prepared.persist_published_cache(published, cache);
    }
    for (const auto& file : fs::directory_iterator(cache)) {
        if (file.path().extension() != ".sqlite") continue;
        const auto bytes = read_file(file.path());
        expect(bytes.find(private_query) == std::string::npos &&
               bytes.find(to_lower(private_query)) == std::string::npos &&
               bytes.find("query_text") == std::string::npos && bytes.find("query_stems") == std::string::npos,
               "query text and normalization tables cannot enter a transferred cache");
    }
    const auto transferred = prepared.preparation_metrics();
    expect(transferred.at("cache_status") == "persisted" && transferred.at("index_builds") == 1 &&
           transferred.at("persist_ms").is_number(), "transfer reports persistence without rebuilding");
    expect(transferred.at("transfer_ms").get<double>() >=
               transferred.at("transfer_verify_ms").get<double>() + transferred.at("transfer_snapshot_ms").get<double>() &&
           transferred.at("transfer_verify_ms").get<double>() >= 0 && transferred.at("transfer_snapshot_ms").get<double>() >= 0,
           "transfer components must fit the measured transfer interval");
    SourceDiscoveryIndex restored(published, cache);
    const auto actual = restored.query(published, {"transferredPublicationNeedle"});
    expect(actual.at("cache").at("status") == "restored" && actual.at("reuse").at("index_builds") == 0 &&
           actual.at("results")[0].at("candidates") == expected.at("results")[0].at("candidates"),
           "published cache restores with query parity");

    // Each failure starts from fresh admissions so an earlier metadata change
    // cannot mask a missing source, profile, dependency or output fence.
    for (const auto* mutation : {"source", "membership", "link", "profile", "candidate-output", "published-output"}) {
        Fixture mutated(parent);
        configure_admission_profile(mutated);
        mutated.write("README.md", "# Transfer fixture\n\n[dependency](target.bin)\n");
        mutated.write("target.bin", "present");
        mutated.write("src/live.sql", content);
        mutated.publish();
        auto stale_candidate = mutated.options;
        stale_candidate.out_root = fs::absolute(mutated.base / "candidate-index");
        const auto stale_generated = generate_outputs(stale_candidate.profile, stale_candidate.root);
        shin::generated_io::publish_snapshot(stale_candidate.out_root, kGeneratedDir, stale_generated.files);
        stale_candidate.generation_admission = GenerationAdmission::reuse(stale_candidate,
            generation_fingerprint(stale_candidate));
        auto stale_published = mutated.options;
        stale_published.generation_admission = stale_candidate.generation_admission->published_copy(
            stale_candidate, stale_published);
        SourceDiscoveryIndex stale_prepared(stale_candidate);
        const std::string change(mutation);
        if (change == "membership") mutated.write("src/added.sql", "SELECT added;\n");
        else if (change == "link") fs::remove(mutated.options.root / "target.bin");
        else {
            const auto path = change == "source" ? mutated.options.root / "src/live.sql" :
                change == "profile" ? fs::path(mutated.options.profile.config_source) :
                (change == "candidate-output" ? stale_candidate.out_root : stale_published.out_root) /
                    kGeneratedDir / "documents.jsonl";
            const auto mtime = fs::last_write_time(path);
            auto bytes = read_file(path);
            bytes.at(0) = bytes.at(0) == 'x' ? 'y' : 'x';
            { std::ofstream stream(path, std::ios::binary | std::ios::trunc); stream << bytes; }
            fs::last_write_time(path, mtime);
        }
        const auto refused_cache = fs::absolute(mutated.base / "published-cache");
        rejected([&] { stale_prepared.persist_published_cache(stale_published, refused_cache); });
        expect(!fs::exists(refused_cache), "changed closure persisted a source cache");
    }
}
}  // namespace
}  // namespace shin::context

int main(int argc, char** argv) {
    try {
        if (argc != 2) return 2;
        shin::context::check(argv[1]);
        shin::context::natural_language_checks(argv[1]);
        shin::context::resident_checks(argv[1]);
        shin::context::snapshot_checks(argv[1]);
        shin::context::context_checks(argv[1]);
        shin::context::disk_checks(argv[1]);
        shin::context::published_cache_transfer_checks(argv[1]);
        std::cout << "source discovery: source binding, selection, limits and redaction passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
