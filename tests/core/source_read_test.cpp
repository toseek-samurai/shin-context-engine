#include "context_internal.h"
#include "generated_io.h"
#include "generated_snapshot.h"
#include "source_read.h"

#include <chrono>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/stat.h>
#endif

namespace shin::context {
namespace {

void expect(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error("source-read: " + message);
}

template <typename Callback>
void rejects(Callback callback, const std::string& code = "") {
    try { callback(); }
    catch (const CliError& error) {
        expect(code.empty() || error.code() == code, "expected " + code + ", got " + error.code());
        return;
    }
    throw std::runtime_error("source-read: expected refusal " + code);
}

class Fixture {
public:
    explicit Fixture(const fs::path& parent) : base(parent / ("source-read-" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()))) {
        options.root = fs::absolute(base / "source");
        options.out_root = fs::absolute(base / "index");
        options.profile.id = "source-read-fixture";
        options.profile.config_hash = "source-read-fixture-v1";
        options.profile.root_files = {"README.md"};
        options.profile.source_fact_roots = {"src"};
        options.profile.source_fact_extensions = {".cpp"};
        options.profile.source_fact_extractors = {"language"};
        options.profile.source_fact_max_file_bytes = 2 * 1024 * 1024;
        write("README.md", "# Source read fixture\n\nCurrent input.\n");
    }
    ~Fixture() { std::error_code error; fs::remove_all(base, error); }
    void write(const fs::path& path, const std::string& content) const {
        fs::create_directories((options.root / path).parent_path());
        std::ofstream stream(options.root / path, std::ios::binary);
        stream << content;
        stream.close();
        expect(!stream.fail(), "write fixture");
    }
    void publish(const std::function<void(Json&)>& change = {}) {
        auto generated = generate_outputs(options.profile, options.root);
        const auto fact_key = (kGeneratedDir / "facts.jsonl").generic_string();
        std::istringstream stream(generated.files.at(fact_key));
        std::string line, facts;
        rows.clear();
        while (std::getline(stream, line)) {
            auto row = Json::parse(line);
            if (change) change(row);
            rows.push_back(row);
            facts += row.dump() + "\n";
        }
        if (change) generated.files[fact_key] = facts;
        const auto manifest_key = (kGeneratedDir / "manifest.json").generic_string();
        auto manifest = Json::parse(generated.files.at(manifest_key));
        for (auto& entry : manifest["generated_file_integrity"]["files"]) {
            if (change && entry.at("path") == fact_key) {
                entry["bytes"] = facts.size();
                entry["content_hash"] = fnv1a64(facts);
            }
        }
        if (change) generated.files[manifest_key] = manifest.dump(2) + "\n";
        shin::generated_io::IndexLock lock(options.out_root, shin::generated_io::LockMode::exclusive,
                                           shin::generated_io::LockTarget::writer);
        shin::generated_io::publish_snapshot(options.out_root, kGeneratedDir, generated.files);
    }
    std::string id(const std::string& symbol) const {
        for (const auto& row : rows)
            if (json_string_value(row, "symbol_name") == symbol) return row.at("id");
        throw std::runtime_error("source-read: missing fixture symbol " + symbol);
    }
    Json request(const RetrievalSession& session, const std::vector<std::string>& ids) const {
        return {{"schema_version", "shin.source-read-request.v1"},
            {"snapshot_id", session.freshness().snapshot_id}, {"record_ids", ids}};
    }
    fs::path base;
    RuntimeOptions options;
    std::vector<Json> rows;
};

void objective_c_bodies(const fs::path& parent) {
    Fixture fixture(parent);
    fixture.options.profile.source_fact_extensions = {".mm", ".h"};
    const std::string body = "- (int)render:(int)value {\r\n return value + 1;\r\n}\r\n";
    const std::string native = "@implementation Panel\r\n" + body + "@end\r\n";
    const std::string fragment_body = "- (void)connect:(id)sender {\r\n (void)sender;\r\n}\r\n";
    fixture.write("src/native.mm", native);
    fixture.write("src/implementation.h", "// included inside an implementation\r\n" + fragment_body);
    fixture.publish();
    const auto snapshot = GeneratedSnapshot::load(fixture.options);
    RetrievalSession disk(fixture.options, RetrievalSessionOpenMode::verified_snapshot), resident(snapshot, false);
    for (const auto* path : {"src/native.mm", "src/implementation.h"})
        expect(disk.inspect(path) == resident.inspect(path), "Objective-C++ outline cold/warm parity");
    const auto request = fixture.request(disk, {fixture.id("render:"), fixture.id("connect:")});
    const auto response = disk.read(request.dump());
    expect(response == resident.read(request.dump()) && response.at("complete") == true,
           "Objective-C++ read cold/warm parity");
    const auto& evidence = response.at("evidence");
    expect(evidence.at("bodies")[0].at("text") == body &&
           evidence.at("bodies")[1].at("text") == fragment_body, "original Objective-C++ method bytes and ranges");
    for (const auto& row : evidence.at("records"))
        expect(row.at("range_origin") == "syntax" && row.at("verification_level") == "source-observed",
               "Objective-C++ source provenance");
    expect(fixture.id("render:").ends_with("-b" + std::to_string(native.find("render:"))),
           "Objective-C++ fact identity uses the original name offset");
}

void exact_bodies(const fs::path& parent) {
    Fixture fixture(parent);
    const std::string first = "int alpha() {\r\n return 3; // quote: \" and \\ and \t\r\n}\r\n";
    const std::string last = "int omega() { return 4; }";
    fixture.write("src/space name.cpp", first + last);
    const std::string shared_source = "int left() { return 1; } int right() { return 2; }\n";
    fixture.write("src/shared.cpp", shared_source);
    fixture.publish();
    auto snapshot = GeneratedSnapshot::load(fixture.options);
    RetrievalSession disk(fixture.options, RetrievalSessionOpenMode::verified_snapshot);
    RetrievalSession resident(snapshot, false);
    const auto request = fixture.request(disk, {fixture.id("alpha"), fixture.id("omega"), fixture.id("alpha")});
    const auto output = disk.read(request.dump());
    expect(output == resident.read(request.dump()), "disk/resident response parity");
    expect(output.at("complete") == true && output.at("requested_records") == 3 && output.at("unique_records") == 2,
           "duplicates retained as request positions only");
    const auto& evidence = output.at("evidence");
    expect(evidence.at("bodies").size() == 2 && evidence.at("records").size() == 2, "unique bodies");
    expect(evidence.at("bodies")[0].at("text") == first && evidence.at("bodies")[1].at("text") == last,
           "exact physical bytes, CRLF, tabs, quotes and missing terminal newline");
    expect(evidence.at("records")[0].at("request_indices") == Json::array({0, 2}), "duplicate provenance");
    for (const auto& row : evidence.at("records")) {
        expect(row.at("verification_level") == "source-observed", "no trust escalation");
        expect(row.at("range_origin") == "syntax", "range origin preserved");
    }
    expect(output.at("evidence_bytes") == evidence.dump().size(), "serialized evidence accounting");
    expect(disk.read(fixture.request(disk, std::vector<std::string>(8, fixture.id("alpha"))).dump()).at("complete") == true,
           "exact eight-selection bound includes duplicates");
    const auto shared = disk.read(fixture.request(disk, {fixture.id("left"), fixture.id("right")}).dump());
    expect(shared.at("evidence").at("bodies").size() == 1 && shared.at("evidence").at("records").size() == 2,
           "same physical range shares body without merging record provenance");
    expect(shared.at("materialization_source_bytes") == shared_source.size(), "one physical source read");
}

void request_checks(const fs::path& parent) {
    Fixture fixture(parent);
    fixture.write("src/a.cpp", "int alpha() { return 1; }\n");
    fixture.publish();
    RetrievalSession session(fixture.options, false);
    const auto request = fixture.request(session, {fixture.id("alpha")});
    for (const std::string text : {"", "{", "[]", "null"}) rejects([&] { (void)session.read(text); });
    rejects([&] { (void)session.read(std::string(128 * 1024 + 1, ' ')); }, "source-read-input-too-large");
    for (const auto& [field, value] : std::vector<std::pair<std::string, Json>>{
        {"path", "../outside"}, {"start_line", 1}, {"record_ids", Json::array()},
        {"record_ids", Json::array({1})}, {"snapshot_id", "invalid"}, {"schema_version", "wrong.v1"}}) {
        auto bad = request;
        bad[field] = value;
        rejects([&] { (void)session.read(bad.dump()); });
    }
    auto bad = request;
    bad["snapshot_id"] = "fnv1a64:0000000000000000";
    rejects([&] { (void)session.read(bad.dump()); }, "source-read-snapshot-mismatch");
    bad = request;
    bad["record_ids"] = {"unknown"};
    rejects([&] { (void)session.read(bad.dump()); }, "source-read-unknown-record");
    bad["record_ids"] = std::vector<std::string>(9, fixture.id("alpha"));
    rejects([&] { (void)session.read(bad.dump()); }, "source-read-record-budget");
    const std::string duplicate = request.dump().substr(0, request.dump().size() - 1) + ",\"record_ids\":[]}";
    rejects([&] { (void)session.read(duplicate); }, "source-read-invalid-json");
    rejects([&] { (void)session.read(std::string(34, '[') + std::string(34, ']')); }, "source-read-invalid-json");
}

void selected_snapshot(const fs::path& parent) {
    Fixture fixture(parent);
    const std::string content = "int alpha() { return 1; }\n// outside selected range\n";
    fixture.write("src/a.cpp", content);
    fixture.write("src/unrelated.cpp", "int unrelated() { return 2; }\n");
    fixture.publish();
    const auto snapshot = GeneratedSnapshot::load(fixture.options);
    RetrievalSession resident(snapshot, false);
    RetrievalSession disk(fixture.options, RetrievalSessionOpenMode::verified_snapshot);
    const auto request = fixture.request(resident, {fixture.id("alpha")});
    rejects([&] { (void)disk.read_selected(request.dump()); }, "invalid-snapshot-session");
    auto wrong = request;
    wrong["snapshot_id"] = "fnv1a64:0000000000000000";
    rejects([&] { (void)resident.read_selected(wrong.dump()); }, "source-read-snapshot-mismatch");
    auto missing = request;
    missing.erase("snapshot_id");
    rejects([&] { (void)resident.read_selected(missing.dump()); });
    const auto strict = resident.read(request.dump());
    fixture.write("src/unrelated.cpp", "int changed_unrelated() { return 3; }\n");
    fixture.write("src/added.cpp", "int added() { return 4; }\n");
    const auto count = generation_execution_count();
    const auto selected = resident.read_selected(request.dump());
    expect(selected.at("evidence") == strict.at("evidence"), "selected materialization retains strict evidence");
    expect(generation_execution_count() == count, "selected read never regenerates the corpus");
    const auto& freshness = selected.at("index_freshness");
    expect(freshness.at("status") == "not-assessed" && freshness.at("current") == false &&
           freshness.at("fully_verified") == false && selected.at("snapshot_id") == snapshot->id(),
           "selected source verification is not global freshness");
    expect(!resident.freshness().current && !resident.freshness().fully_verified,
           "selected read does not mutate session freshness");
    const Json files = {{"schema_version", "shin.source-read-request.v2"},
        {"snapshot_id", snapshot->id()}, {"file_paths", {"src/a.cpp"}}};
    expect(resident.read_selected(files.dump()).at("evidence").at("bodies")[0].at("text") == content,
           "selected whole-file read ignores unrelated changes");
    rejects([&] { (void)resident.read(request.dump()); });
    fixture.write("src/a.cpp", content + "// change outside the selected range\n");
    rejects([&] { (void)resident.read_selected(request.dump()); }, "source-read-hash-mismatch");
    rejects([&] { (void)resident.read_selected(files.dump()); }, "source-read-hash-mismatch");
    fixture.write("src/a.cpp", content);
    fs::remove(fixture.options.root / "src/a.cpp");
    rejects([&] { (void)resident.read_selected(request.dump()); }, "source-read-unavailable");
    rejects([&] { (void)resident.read_selected(files.dump()); }, "source-read-unavailable");
    fs::create_symlink(fixture.options.root / "src/unrelated.cpp", fixture.options.root / "src/a.cpp");
    rejects([&] { (void)resident.read_selected(request.dump()); });
}

void range_and_path_safety(const fs::path& parent) {
    for (const std::string mutation : {"hash", "range", "path"}) {
        Fixture fixture(parent);
        fixture.write("src/a.cpp", "int alpha() { return 1; }\n");
        fixture.publish([&](Json& row) {
            if (mutation == "hash") row["span_hash"] = "fnv1a64:0000000000000000";
            if (mutation == "range") row["end_line"] = 999;
            if (mutation == "path") row["path"] = "../outside.cpp";
        });
        RetrievalSession session(fixture.options, RetrievalSessionOpenMode::verified_snapshot);
        const auto request = fixture.request(session, {fixture.id("alpha")});
        rejects([&] { (void)session.read(request.dump()); });
    }
    Fixture fixture(parent);
    const std::string content = "int alpha() { return 1; }\n";
    fixture.write("src/a.cpp", content);
    fixture.publish();
    const auto snapshot = GeneratedSnapshot::load(fixture.options);
    Json request;
    {
        RetrievalSession session(snapshot, false);
        request = fixture.request(session, {fixture.id("alpha")});
        fixture.write("src/a.cpp", "int alpha() { return 2; }\n");
        rejects([&] { (void)session.read(request.dump()); });
        fixture.write("src/a.cpp", content);
        fixture.write("outside.cpp", content);
        fs::remove(fixture.options.root / "src/a.cpp");
        fs::create_symlink(fixture.options.root / "outside.cpp", fixture.options.root / "src/a.cpp");
        rejects([&] { (void)session.read(request.dump()); });
        rejects([&] { (void)read_bound_source(fixture.options.root, "src/a.cpp", fnv1a64(content), 1000); });
        fs::remove(fixture.options.root / "src/a.cpp");
        fixture.write("src/a.cpp", content);
        fs::rename(fixture.options.root / "src", fixture.options.root / "actual");
        fs::create_directory_symlink(fixture.options.root / "actual", fixture.options.root / "src");
        rejects([&] { (void)session.read(request.dump()); });
        rejects([&] { (void)read_bound_source(fixture.options.root, "src/a.cpp", fnv1a64(content), 1000); });
        fs::remove(fixture.options.root / "src");
        fs::rename(fixture.options.root / "actual", fixture.options.root / "src");
    }
    fixture.write("src/a.cpp", "int replacement() { return 7; }\n");
    fixture.publish();
    RetrievalSession old(snapshot, false);
    rejects([&] { (void)old.read(request.dump()); }, "source-read-snapshot-mismatch");
#ifdef _WIN32
    const auto name = "shin-source-read-" + std::to_string(GetCurrentProcessId());
    const auto pipe_root = fs::path(L"\\\\.\\pipe");
    const auto pipe = CreateNamedPipeW((pipe_root / name).c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE | PIPE_WAIT, 1, 512, 512, 0, nullptr);
    expect(pipe != INVALID_HANDLE_VALUE, "create special file");
    struct Close { HANDLE value; ~Close() { CloseHandle(value); } } close{pipe};
    rejects([&] { (void)read_bound_source(pipe_root, name, fnv1a64(""), 1000); }, "unsafe-execution-file");
#else
    expect(::mkfifo((fixture.options.root / "pipe").c_str(), 0600) == 0, "create special file");
    rejects([&] { (void)read_bound_source(fixture.options.root, "pipe", fnv1a64(""), 1000); }, "unsafe-execution-file");
#endif
}

void redaction_and_limits(const fs::path& parent) {
    Fixture fixture(parent);
    const std::string secret = "sk-" + std::string(64, 'Q');
    const std::string body = "namespace Sensitive {\nint clean() { return 1; }\nint credential() {\n/*" +
        std::string(5000, 'x') + "*/\nconst char* token = \"" + secret + "\"; // gitleaks:allow\nreturn 2;\n}\n}\n";
    fixture.write("src/sensitive.cpp", body);
    fixture.write("src/large.cpp", "int oversized() {\n/*" + std::string(40000, 'q') + "*/\nreturn 4;\n}\n");
    fixture.write("src/ok.cpp", "int normal() { return 3; }\n");
    fixture.write("src/escaped.cpp", "int escaped() {\n/*" + std::string(20000, '\\') + "*/\nreturn 4;\n}\n");
    fixture.publish();
    RetrievalSession session(fixture.options, true);
    const Json quarantined_request = {{"schema_version", "shin.source-read-request.v2"},
        {"snapshot_id", session.freshness().snapshot_id},
        {"file_paths", {"src/sensitive.cpp", "src/large.cpp", "src/ok.cpp", "src/escaped.cpp"}}};
    const auto output = session.read(quarantined_request.dump());
    RetrievalSession resident(GeneratedSnapshot::load(fixture.options), false);
    expect(resident.read_selected(quarantined_request.dump()).at("evidence") ==
        output.at("evidence"), "selected read preserves overlap redaction and output bounds");
    expect(output.at("ok") == true && output.at("complete") == false, "valid incomplete response");
    expect(output.dump().find(secret) == std::string::npos, "secret beyond preview not disclosed");
    const auto& evidence = output.at("evidence");
    expect(evidence.at("omissions").size() == 2 && evidence.at("omissions")[0].at("reason") == "serialized-evidence-budget" &&
           evidence.at("omissions")[1].at("reason") == "serialized-evidence-budget",
           "whole oversized bodies omitted, including JSON escaping cost");
    int redacted = 0;
    for (const auto& span : evidence.at("bodies")) {
        if (span.at("state") == "redacted") {
            ++redacted;
            expect(span.at("text").is_null(), "redaction is not a clipped body");
        }
    }
    expect(redacted == 1, "quarantined file has no body, including its clean declarations");
    expect(evidence.dump().size() <= 32768 && output.dump().size() + 1 <= 131072, "serialized hard caps");

    Fixture files(parent);
    for (int i = 0; i < 5; ++i) files.write("src/f" + std::to_string(i) + ".cpp", "int f" + std::to_string(i) + "() { return 1; }\n");
    files.publish();
    RetrievalSession five(files.options, false);
    rejects([&] { (void)five.read(files.request(five, {files.id("f0"), files.id("f1"), files.id("f2"), files.id("f3"), files.id("f4")}).dump()); },
            "source-read-file-budget");

    Fixture large(parent);
    large.write("src/huge.cpp", "int huge() {\n/*" + std::string(1024 * 1024, 'x') + "*/\nreturn 1;\n}\n");
    large.publish();
    RetrievalSession huge(large.options, false);
    const auto limited = huge.read(large.request(huge, {large.rows.front().at("id").get<std::string>()}).dump());
    expect(limited.at("complete") == false && limited.at("materialization_source_bytes") == 0,
           "oversized source not materialized");
    expect(limited.at("evidence").at("omissions")[0].at("reason") == "source-file-too-large", "explicit file cap");
}

void concurrent_source_change(const fs::path& parent, bool whole_file, bool selected_only = false) {
    Fixture fixture(parent);
    const std::string original = "int alpha() {\n/*" + std::string(128 * 1024, 'x') + "*/\nreturn 1;\n}\n";
    fixture.write("src/a.cpp", original);
    fixture.publish();
    const auto snapshot = GeneratedSnapshot::load(fixture.options);
    RetrievalSession session(snapshot, false);
    const Json request = whole_file ? Json{{"schema_version", "shin.source-read-request.v2"},
        {"snapshot_id", session.freshness().snapshot_id}, {"file_paths", {"src/a.cpp"}}}
        : fixture.request(session, {fixture.id("alpha")});
#ifdef _WIN32
    // Windows denies a reader while a writable handle exists, before bytes race.
    const auto writer = CreateFileW((fixture.options.root / "src/a.cpp").c_str(), GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    expect(writer != INVALID_HANDLE_VALUE, "open concurrent source writer");
    struct Close { HANDLE value; ~Close() { CloseHandle(value); } } close{writer};
    rejects([&] {
        if (selected_only) (void)session.read_selected(request.dump());
        else (void)session.read(request.dump());
    });
#else
    // Thread creation does not guarantee a write before the read. Establish the
    // changed source first, and never restore a legitimately readable original.
    const auto changed_a = "// concurrent edit A\n" + original;
    const auto changed_b = "// concurrent edit B\n" + original;
    fixture.write("src/a.cpp", changed_a);
    std::jthread writer([&](std::stop_token stop) {
        while (!stop.stop_requested()) {
            fixture.write("src/a.cpp", changed_b);
            fixture.write("src/a.cpp", changed_a);
        }
    });
    bool refused = false;
    try {
        if (selected_only) (void)session.read_selected(request.dump());
        else (void)session.read(request.dump());
    }
    catch (const CliError&) { refused = true; }
    writer.request_stop();
    writer.join();
    expect(refused, "concurrent source writer refuses instead of returning mixed bytes");
#endif
}

Json file_request(const RetrievalSession& session, const std::vector<std::string>& paths) {
    return {{"schema_version", "shin.source-read-request.v2"},
            {"snapshot_id", session.freshness().snapshot_id}, {"file_paths", paths}};
}

void whole_files(const fs::path& parent) {
    Fixture fixture(parent);
    const std::string content = "// outside declaration\r\nconstexpr int limit = 16;\r\nint alpha() { return limit; }";
    fixture.write("src/space name.cpp", content);
    fixture.write("src/empty.cpp", "");
    fixture.write("src/newline.cpp", "\n");
    fixture.publish();
    RetrievalSession session(fixture.options, RetrievalSessionOpenMode::verified_snapshot);
    RetrievalSession resident(GeneratedSnapshot::load(fixture.options), false);
    const auto request = file_request(session, {"src/space name.cpp", "src/space name.cpp", "src/empty.cpp", "src/newline.cpp"});
    const auto output = session.read(request.dump());
    expect(output == resident.read(request.dump()), "file disk/resident parity");
    expect(output.at("schema_version") == "shin.source-read.v2" && output.at("complete") == true,
           "separate whole-file result schema");
    expect(output.at("requested_files") == 4 && output.at("unique_files") == 3, "duplicate file selection positions");
    const auto& evidence = output.at("evidence");
    expect(!evidence.contains("records") && evidence.at("files").size() == 3, "file metadata is not invented fact metadata");
    expect(evidence.at("files")[0].at("request_indices") == Json::array({0, 1}), "file request positions");
    const auto& bodies = evidence.at("bodies");
    expect(bodies[0].at("text") == content && bodies[0].at("end_line") == 3, "whole bytes include non-declaration content");
    expect(bodies[1].at("text") == "" && bodies[1].at("end_line") == 1, "empty file addressable as empty first line");
    expect(bodies[2].at("text") == "\n" && bodies[2].at("end_line") == 1, "no phantom trailing line");
    expect(output.at("evidence_bytes") == evidence.dump().size(), "whole-file byte accounting");
    for (const std::string path : {"README.md", "src/unknown.cpp", "../outside.cpp", "/absolute.cpp",
                                   "src/../src/empty.cpp", "src//empty.cpp", "src/empty.cpp/", "src/./empty.cpp"})
        rejects([&] { (void)session.read(file_request(session, {"src/space name.cpp", path}).dump()); });
    for (const auto& paths : {std::vector<std::string>{}, std::vector<std::string>(5, "src/empty.cpp")})
        rejects([&] { (void)session.read(file_request(session, paths).dump()); }, "source-read-file-budget");
    auto mixed = request;
    mixed["record_ids"] = {fixture.id("alpha")};
    rejects([&] { (void)session.read(mixed.dump()); }, "source-read-invalid-request");
    mixed = request;
    mixed["schema_version"] = "shin.source-read-request.v1";
    rejects([&] { (void)session.read(mixed.dump()); }, "source-read-invalid-request");
    auto wrong = request;
    wrong["snapshot_id"] = "fnv1a64:0000000000000000";
    rejects([&] { (void)session.read(wrong.dump()); }, "source-read-snapshot-mismatch");
    fixture.write("src/space name.cpp", content + "\n// stale");
    rejects([&] { (void)session.read(request.dump()); });
    fixture.write("src/space name.cpp", content);
    fs::remove(fixture.options.root / "src/space name.cpp");
    fs::create_symlink(fixture.options.root / "src/empty.cpp", fixture.options.root / "src/space name.cpp");
    rejects([&] { (void)session.read(request.dump()); });
}

void whole_file_redaction_and_limits(const fs::path& parent) {
    Fixture fixture(parent);
    const std::string secret = "sk-" + std::string(64, 'Q');
    fixture.write("src/secret.cpp", "int clean() { return 1; }\n/*" + std::string(5000, 'x') +
                  "*/\n// token = \"" + secret + "\"; // gitleaks:allow\n");
    fixture.write("src/escaped.cpp", "//" + std::string(20000, '\\') + "\nint escaped() { return 2; }\n");
    fixture.write("src/huge.cpp", "//" + std::string(1024 * 1024, 'q') + "\nint huge() { return 3; }\n");
    fixture.write("src/ok.cpp", "int ok() { return 4; }\n");
    fixture.publish();
    RetrievalSession session(fixture.options, false);
    const auto output = session.read(file_request(session, {
        "src/secret.cpp", "src/escaped.cpp", "src/huge.cpp", "src/ok.cpp"}).dump());
    expect(output.at("complete") == false && output.dump().find(secret) == std::string::npos,
           "secret outside declaration redacts whole file");
    const auto& evidence = output.at("evidence");
    expect(evidence.at("bodies")[0].at("text").is_null(), "whole secret body absent");
    expect(evidence.at("bodies")[1].at("text") == "int ok() { return 4; }\n", "later fitting file retained");
    expect(evidence.at("omissions").size() == 2 &&
           evidence.at("omissions")[0].at("reason") == "serialized-evidence-budget" &&
           evidence.at("omissions")[1].at("reason") == "source-file-too-large", "whole file omissions not clipping");
    expect(evidence.dump().size() <= 32768 && output.dump().size() + 1 <= 131072, "file result hard caps");
}

}  // namespace
}  // namespace shin::context

int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::runtime_error("expected fixture parent");
        shin::context::exact_bodies(argv[1]);
        shin::context::objective_c_bodies(argv[1]);
        shin::context::request_checks(argv[1]);
        shin::context::selected_snapshot(argv[1]);
        shin::context::range_and_path_safety(argv[1]);
        shin::context::redaction_and_limits(argv[1]);
        shin::context::concurrent_source_change(argv[1], false);
        shin::context::concurrent_source_change(argv[1], true);
        shin::context::concurrent_source_change(argv[1], false, true);
        shin::context::concurrent_source_change(argv[1], true, true);
        shin::context::whole_files(argv[1]);
        shin::context::whole_file_redaction_and_limits(argv[1]);
        std::cout << "source-read: exact bytes, admission, parity, redaction and bounds passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
