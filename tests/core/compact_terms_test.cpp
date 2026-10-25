#include "compact_terms.h"
#include "context_internal.h"
#include "generated_io.h"
#include "generated_row_decoder.h"
#include "generated_snapshot.h"
#include "retrieval_session.h"
#include "agent_context_pack.h"
#include <iostream>
#include <random>
#ifndef _WIN32
#include <sys/stat.h>
#endif

using namespace shin::context;
namespace {
void expect(bool ok, const std::string& message) { if (!ok) throw std::runtime_error(message); }
bool same(const TermPosting& a, const TermPosting& b) {
    return a.sections == b.sections && a.facts == b.facts && a.decisions == b.decisions &&
        a.contracts == b.contracts && a.runbooks == b.runbooks;
}
void reject(const std::string& bytes, TermRowCounts counts) {
    try { std::istringstream input(bytes); CompactTerms terms(input, counts); terms.validate_all(); }
    catch (const CliError& error) { expect(error.code() == "index-corrupt", "wrong corruption code"); return; }
    throw std::runtime_error("malformed lexical bytes admitted");
}
class CountingBuffer : public std::stringbuf {
public:
    explicit CountingBuffer(const std::string& bytes) : std::stringbuf(bytes) {}
    std::size_t seeks = 0, reads = 0, largest_read = 0;
protected:
    pos_type seekpos(pos_type p, std::ios_base::openmode mode) override {
        ++seeks; return std::stringbuf::seekpos(p, mode);
    }
    pos_type seekoff(off_type p, std::ios_base::seekdir direction, std::ios_base::openmode mode) override {
        ++seeks; return std::stringbuf::seekoff(p, direction, mode);
    }
    std::streamsize xsgetn(char* target, std::streamsize count) override {
        ++reads; largest_read = std::max(largest_read, static_cast<std::size_t>(count));
        return std::stringbuf::xsgetn(target, count);
    }
};
void block_validation() {
    TermPostingMap map;
    for (int i = 0; i < 20000; ++i) map["bounded-term-" + std::to_string(i)].sections = {0, 2};
    const TermRowCounts counts{70000, 0, 0, 0, 0};
    const auto bytes = encode_compact_terms(map, counts);
    CountingBuffer buffer(bytes); std::istream input(&buffer); CompactTerms terms(input, counts);
    terms.validate_all();
    expect(buffer.seeks < 100 && buffer.reads < 100, "validation performs per-term I/O");
    expect(buffer.largest_read <= 64U * 1024U, "validation read-ahead is not bounded");
    const auto reads = buffer.reads;
    terms.validate_all();
    expect(buffer.reads > reads, "validation reused stale cross-call buffers");
    expect(same(terms.posting(terms.lower_bound("bounded-term-9999")), map.at("bounded-term-9999")),
           "random access after full validation differs");
    auto corrupt_tail = bytes; corrupt_tail.back() = char(127); reject(corrupt_tail, counts);
    auto corrupt_offsets = bytes; corrupt_offsets[32 + 8192 * 8] = 0;
    // Force an invalid offset exactly beyond the first table window.
    for (int i = 0; i < 8; ++i) corrupt_offsets[32 + 8192 * 8 + i] = char(255);
    reject(corrupt_offsets, counts);
    map.clear();
    auto& large = map["large"].sections;
    for (int i = 0; i < 70000; ++i) large.push_back(i);
    const auto big = encode_compact_terms(map, counts);
    std::istringstream big_input(big); CompactTerms large_terms(big_input, counts);
    large_terms.validate_all();
    expect(same(large_terms.posting(0), map.at("large")), "large posting fallback differs");
}
void codec() {
    block_validation();
    TermPostingMap map{{"alpha", {{0, 1, 128, 4000}, {0, 299}, {}, {3}, {0}}},
                       {"alphabet", {{2}, {}, {0}, {}, {}}}, {"βeta", {{12}, {}, {}, {}, {}}}};
    TermRowCounts counts{5000, 300, 1, 4, 1};
    const auto bytes = encode_compact_terms(map, counts);
    std::istringstream input(bytes); CompactTerms terms(input, counts); terms.validate_all();
    expect(terms.size() == map.size(), "wrong dictionary count");
    for (const auto& [key, posting] : map) {
        const auto id = terms.lower_bound(key);
        expect(terms.term(id) == key && same(terms.posting(id), posting), "posting roundtrip differs");
    }
    expect(terms.lower_bound("") == 0 && terms.lower_bound("alphaa") == 1 &&
           terms.lower_bound("zz") == 2, "lower bound differs");
    for (std::size_t length = 0; length < bytes.size(); ++length) reject(bytes.substr(0, length), counts);
    reject(bytes + "x", counts);
    auto damaged = bytes; damaged[0] = 'X'; reject(damaged, counts);
    damaged = bytes; damaged[8] = char(255); reject(damaged, counts);
    damaged = bytes; damaged[32] = 0; reject(damaged, counts);
    damaged = bytes; damaged[40] = 0; reject(damaged, counts);
    auto wrong_counts = counts; ++wrong_counts[0]; reject(bytes, wrong_counts);
    // First record starts at 64: len=5 + alpha + count=4 + first delta=1.
    damaged = bytes; damaged[71] = 0; reject(damaged, counts);
    damaged = bytes; damaged[70] = char(127); reject(damaged, counts);
    damaged = bytes; damaged[64] = char(0x85); damaged[65] = 0; reject(damaged, counts);
    damaged = bytes; damaged[64] = char(0xff); damaged[65] = char(0xff);
    damaged[66] = char(0xff); damaged[67] = char(0xff); damaged[68] = char(0x08); reject(damaged, counts);
    damaged = bytes; damaged[65] = char(0xff); reject(damaged, counts);
    damaged = bytes; damaged[65] = 'z'; reject(damaged, counts); // out-of-order keys
    std::mt19937 random(9137);
    map.clear();
    for (unsigned key = 0; key < 500; ++key) {
        auto& posting = map["key-" + std::to_string(key)];
        for (int row = 0; row < 5000; ++row) if (random() % 101 == 0) posting.sections.push_back(row);
    }
    const auto randomized = encode_compact_terms(map, counts);
    std::istringstream random_input(randomized); CompactTerms decoded(random_input, counts);
    decoded.validate_all();
    for (const auto& [key, posting] : map) expect(same(decoded.posting(decoded.lower_bound(key)), posting), "random parity");
    for (const auto* generator : {"shin-generator-v21-cpp-syntax", "shin-generator-v22-compact-lexical"}) {
        bool refused = false;
        try { (void)compact_lexical_manifest({{"generator_version", generator}, {"term_index_format", "shin.lexical.v1"}}); }
        catch (const CliError& error) { refused = error.code() == "index-generator-unsupported"; }
        expect(refused, "retired generator accepted for retrieval");
    }
    for (const auto& marker : {Json(nullptr), Json(123), Json("")}) {
        bool refused = false;
        try { (void)compact_lexical_manifest({{"generator_version", kGeneratorVersion}, {"term_index_format", marker}}); }
        catch (const CliError&) { refused = true; }
        expect(refused, "invalid format marker accepted");
    }
    for (const auto& count : {Json(-1), Json(4294967296ULL), Json(1.5)}) {
        bool refused = false;
        try { (void)compact_term_count({{"row_counts", {{"terms", count}}}}); }
        catch (const CliError&) { refused = true; }
        expect(refused, "invalid manifest term count accepted");
    }
    // Small compressed input cannot inflate beyond the old logical row budget.
    constexpr std::uint32_t large_count = 10000000;
    auto inflated = encode_compact_terms({{"aa", {{0}, {}, {}, {}, {}}}}, {large_count, 0, 0, 0, 0});
    inflated.resize(48); // header and two offsets
    inflated += std::string(1, char(2)) + "aa";
    for (unsigned char byte : {0x80, 0xad, 0xe2, 0x04}) inflated.push_back(static_cast<char>(byte));
    inflated.append(large_count, char(1));
    inflated.append(4, char(0));
    for (unsigned byte = 0; byte < 8; ++byte)
        inflated[40 + byte] = static_cast<char>((static_cast<std::uint64_t>(inflated.size()) >> (8U * byte)) & 255U);
    reject(inflated, {large_count, 0, 0, 0, 0});
    const auto empty = encode_compact_terms({}, {});
    std::istringstream empty_input(empty); CompactTerms empty_terms(empty_input, {});
    empty_terms.validate_all(); expect(empty_terms.size() == 0, "empty index");
}
void write(const fs::path& path, const std::string& data) {
    fs::create_directories(path.parent_path()); std::ofstream output(path, std::ios::binary); output << data;
    expect(static_cast<bool>(output), "fixture write failed");
}
Json comparable(Json value) {
    if (value.is_object()) {
        // These are deliberately different snapshot identities, not retrieval results.
        for (const char* key : {"snapshot_id", "generator_version", "out_root", "receipt_id"}) value.erase(key);
        for (auto& item : value.items()) item.value() = comparable(item.value());
    } else if (value.is_array()) for (auto& item : value) item = comparable(item);
    return value;
}
void parity(const fs::path& base) {
    RuntimeOptions first;
    first.root = base / "root"; first.out_root = base / "first"; first.force_index = true;
    first.profile.id = "compact-fixture"; first.profile.config_hash = "compact-fixture-v1";
    first.profile.root_files = {"README.md"}; first.profile.source_extensions = {".md"};
    write(first.root / "README.md", "# Lexical fixture\n\nalpha transport publishes context with isolation.\n");
    write(first.root / "docs/contract.md", "# Publishing contract\n\nStatus: accepted\n\nalpha publisher checks beta permissions.\n");
    write(first.root / "docs/other.md", "# Reference\n\nalphabet and gamma preserve query boundaries.\n");
    std::string crowded = "# Prefix boundary\n\n";
    for (int i = 0; i < 2100; ++i) {
        if (i % 700 == 0) crowded += "\n\n## Block " + std::to_string(i) + "\n\n";
        crowded += "zzprefix" + std::to_string(i) + " ";
    }
    write(first.root / "docs/prefix.md", crowded + "\n");
    auto compact = first; compact.out_root = base / "compact";
#ifndef _WIN32
    auto unsafe = first; unsafe.out_root = base / "unsafe";
    fs::create_directories(unsafe.out_root / kGeneratedDir);
    const auto manifest = unsafe.out_root / kGeneratedDir / "manifest.json";
    const auto canary = base / "private-canary";
    write(canary, "must-not-be-parsed");
    const auto refused_probe = [&] {
        bool refused = false;
        try { (void)expected_generated_relative_files(unsafe); }
        catch (const CliError& error) { refused = error.code() == "snapshot-file-invalid"; }
        expect(refused, "format probe read an unsafe manifest");
    };
    fs::create_symlink(canary, manifest);
    refused_probe(); fs::remove(manifest);
    expect(::mkfifo(manifest.c_str(), 0600) == 0, "cannot create FIFO fixture");
    refused_probe(); fs::remove(manifest);
#endif
    const auto first_output = generate_outputs(first.profile, first.root);
    const auto new_output = generate_outputs(compact.profile, compact.root);
    for (const auto& [path, data] : first_output.files)
        if (path.find("terms.") == std::string::npos && !path.ends_with("manifest.json"))
            expect(new_output.files.at(path) == data, "nonlexical evidence changed");
    shin::generated_io::publish_snapshot(first.out_root, kGeneratedDir, first_output.files);
    shin::generated_io::publish_snapshot(compact.out_root, kGeneratedDir, new_output.files);
    expect(assess_index_freshness(first, true).current, "first exact freshness failed");
    expect(assess_index_freshness(compact, true).current, "compact exact freshness failed");
    for (const bool resident : {false, true}) {
        std::unique_ptr<RetrievalSession> a, b;
        if (resident) {
            a = std::make_unique<RetrievalSession>(GeneratedSnapshot::load(first), false);
            b = std::make_unique<RetrievalSession>(GeneratedSnapshot::load(compact), false);
        } else {
            a = std::make_unique<RetrievalSession>(first, false);
            b = std::make_unique<RetrievalSession>(compact, false);
        }
        for (const auto& words : std::vector<std::vector<std::string>>{{"alpha"}, {"alph"}, {"beta", "permissions"},
                {"docs/contract.md", "alpha"}, {"missingword"}, {"alpha", "publisher", "beta", "permissions", "context"}}) {
            const auto query = make_command_query_request(first.profile, words, 8, 3);
            for (const auto method : {&RetrievalSession::search, &RetrievalSession::context})
                expect(comparable((a.get()->*method)(query)) == comparable((b.get()->*method)(query)), "query parity failed");
            expect(comparable(a->brief(query)) == comparable(b->brief(query)), "brief parity failed");
        }
        for (const auto& word : {"publis", "zzprefix"}) {
            auto query = make_command_query_request(first.profile, {word, "alpha", "beta", "gamma", "delta", "epsilon", "zeta", "theta"}, 8, 3);
            for (auto& group : query.prepared_term_groups)
                for (auto& term : group) term.allow_prefix = true;
            const auto left = load_generated_query_candidates(a->options(), query.prepared_term_groups, 1);
            const auto right = load_generated_query_candidates(b->options(), query.prepared_term_groups, 1);
            expect(left.term_index_coverage_complete == right.term_index_coverage_complete &&
                left.from_terms_index == right.from_terms_index, "prefix admission differs");
            expect(left.term_index_coverage_complete == (std::string(word) != "zzprefix"), "prefix boundary not exercised");
            expect(comparable(a->context(query)) == comparable(b->context(query)), "prefix result differs");
        }
        const auto request = parse_task_request({{"schema_version", "shin.task-request.v1"}, {"request_id", "format-parity"},
            {"task", {{"objective", "Check publication and permissions"},
                {"must", {{{"id", "publication"}, {"query", "alpha publisher"}},
                           {{"id", "permissions"}, {"query", "beta permissions"}}}}}}}, first.profile);
        auto render = [&](const RetrievalSession& session) {
            const auto pack = execute_agent_context_pack(session.options(), request, session, 1);
            return comparable(render_agent_context_pack(session.options(), {PackProjection::full, false, true}, request, session, pack));
        };
        auto pa = render(*a), pb = render(*b);
        if (pa != pb) { std::cerr << Json::diff(pa, pb).dump(2) << std::endl; }
        expect(pa == pb, "pack parity failed");
    }
}
} // namespace
int main(int argc, char** argv) {
    try {
        (void)argc; (void)argv;
        codec();
        const auto base = fs::temp_directory_path() / ("shin-compact-" + std::to_string(SteadyClock::now().time_since_epoch().count()));
        try { parity(base); } catch (...) { fs::remove_all(base); throw; }
        fs::remove_all(base);
        std::cout << "compact lexical corruption, independent generations, disk/resident and pack parity passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
