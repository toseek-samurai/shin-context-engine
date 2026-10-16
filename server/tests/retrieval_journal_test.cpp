#include "shin/server/retrieval_journal.h"
#include "shin/server/config.h"

#include <array>
#include <exception>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {
using namespace shin::server;

constexpr std::string_view kInstance = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
constexpr std::string_view kRouteFingerprint = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";

void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template <typename Function>
void expect_invalid(Function&& action) {
    try { action(); }
    catch (const ServiceError& error) {
        check(error.status == 500 && error.code == "journal-invalid-batch", "unexpected journal validation error");
        return;
    }
    throw std::runtime_error("expected journal-invalid-batch");
}

Json null_quality() {
    return {{"retrieval_mode", nullptr}, {"returned_records", nullptr}, {"candidate_rows", nullptr},
            {"covered_groups", nullptr}, {"total_groups", nullptr}, {"pack_complete", nullptr},
            {"pack_coverage_complete", nullptr}, {"required_requirements", nullptr},
            {"satisfied_required_requirements", nullptr}};
}

Json full_fact(std::uint64_t sequence = 7) {
    return {
        {"sequence", sequence}, {"operation", "pack"}, {"selector_kind", "publication"},
        {"project_id", "visible"}, {"selector_id", "shared"}, {"observed_at_unix_ms", 1700000000000LL},
        // The collector's outcome is transport metadata; the durable fact derives it from status.
        {"outcome", "success"}, {"status", 200}, {"reason_code", nullptr}, {"last_phase", "finalization"},
        {"elapsed_ns", 15}, {"result_bytes", 321},
        {"phase_ns", {{"validation", 1}, {"selection", 2}, {"admission", 3}, {"execution", 4}, {"finalization", 5}}},
        {"selection", {{"route_id", "primary-route"}, {"workspace_id", "primary-workspace"},
                        {"source_host_id", "fixture-host"}, {"snapshot_id", "fnv1a64:0123456789abcdef"},
                        {"preparation_id", 9}, {"revision", 3}}},
        {"context", {{"profile_id", "server-profile"}, {"profile_fingerprint", "fnv1a64:fedcba9876543210"},
                       {"route_fingerprint", std::string(kRouteFingerprint)}}},
        {"quality", {{"retrieval_mode", "strict"}, {"returned_records", 2}, {"candidate_rows", 4},
                     {"covered_groups", 1}, {"total_groups", 2}, {"pack_complete", false},
                     {"pack_coverage_complete", true}, {"required_requirements", 3},
                     {"satisfied_required_requirements", 2}}},
        {"query_capture", "captured"}, {"request", {{"query", "neutral telemetry fixture"}, {"limit", 3}}},
        {"unexpected_collector_detail", "must-not-reach-the-durable-fact"},
    };
}

Json batch(Json records, std::uint64_t latest = 7) {
    return {{"instance_id", std::string(kInstance)}, {"source_host_id", "fixture-host"},
            {"server_version", "2.0.0-test"}, {"core_version", "2.0.0-core-test"},
            {"producer_epoch", 19}, {"latest_sequence", latest}, {"records", std::move(records)},
            {"unexpected_batch_detail", "must-not-reach-the-durable-batch"}};
}

std::set<std::string> keys(const Json& value) {
    std::set<std::string> result;
    for (const auto& [key, ignored] : value.items()) {
        (void)ignored;
        result.insert(key);
    }
    return result;
}

void neutral_full_fact_is_canonical() {
    const auto source = full_fact();
    const auto normalized = journal::normalize_batch(batch(Json::array({source})));
    check(keys(normalized) == std::set<std::string>{"instance_id", "source_host_id", "server_version", "core_version",
                                                     "producer_epoch", "latest_sequence", "facts"},
          "normalized batch retained raw collector fields");
    check(normalized.at("latest_sequence") == 7 && normalized.at("facts").size() == 1,
          "normalized batch lost sequence metadata");
    const auto& fact = normalized.at("facts").at(0);
    const std::set<std::string> expected{
        "producer_epoch", "instance_id", "sequence", "record_hash", "project_id", "operation", "selector_kind", "selector_id",
        "observed_at_unix_ms", "status", "reason_code", "last_phase", "elapsed_ns", "result_bytes",
        "validation_ns", "selection_ns", "admission_ns", "execution_ns", "finalization_ns", "route_id",
        "workspace_id", "source_host_id", "snapshot_id", "preparation_id", "revision", "profile_id",
        "profile_fingerprint", "route_fingerprint", "retrieval_mode", "returned_records", "candidate_rows",
        "covered_groups", "total_groups", "pack_complete", "pack_coverage_complete", "required_requirements",
        "satisfied_required_requirements", "query_capture", "request_fingerprint", "request"};
    const auto actual = keys(fact);
    check(actual == expected, "normalized fact has a non-canonical durable projection");
    check(!fact.contains("outcome") && !fact.contains("phase_ns") && !fact.contains("selection") &&
              !fact.contains("context") && !fact.contains("unexpected_collector_detail"),
          "raw telemetry-only fields reached the durable projection");
    check(fact.at("profile_fingerprint") == "fnv1a64:fedcba9876543210" &&
              fact.at("snapshot_id") == "fnv1a64:0123456789abcdef" &&
              fact.at("route_fingerprint") == kRouteFingerprint,
          "normalized provenance or prefixed FNV identity differs");
    const auto request_fingerprint = digest_hex(sha256(Json::array({"shin.exact-request.v1", "visible", "pack",
        source.at("request")}).dump()));
    check(fact.at("request_fingerprint") == request_fingerprint && request_fingerprint.size() == 64,
          "captured exact payload fingerprint differs");
    auto hash_material = fact;
    const auto record_hash = hash_material.at("record_hash");
    hash_material.erase("record_hash");
    hash_material.erase("producer_epoch");
    check(record_hash == digest_hex(sha256(hash_material.dump())) && record_hash.get<std::string>().size() == 64,
          "canonical record hash differs");
    check(fact.at("elapsed_ns") == fact.at("validation_ns").get<std::uint64_t>() + fact.at("selection_ns").get<std::uint64_t>() +
              fact.at("admission_ns").get<std::uint64_t>() + fact.at("execution_ns").get<std::uint64_t>() +
              fact.at("finalization_ns").get<std::uint64_t>(),
          "canonical phases do not partition elapsed time");
}

void null_error_fact_is_valid_and_unattributed() {
    auto source = full_fact(2);
    source["operation"] = "brief";
    source["selector_kind"] = "route";
    source["selector_id"] = "primary-route";
    source["outcome"] = "error";
    source["status"] = 400;
    source["reason_code"] = "invalid-request";
    source["last_phase"] = "validation";
    source["elapsed_ns"] = 5;
    source["result_bytes"] = nullptr;
    source["phase_ns"] = {{"validation", 5}, {"selection", nullptr}, {"admission", nullptr},
                            {"execution", nullptr}, {"finalization", nullptr}};
    source["selection"] = nullptr;
    source["context"] = nullptr;
    source["quality"] = null_quality();
    source["query_capture"] = "invalid-request";
    source["request"] = nullptr;
    const auto fact = journal::normalize_batch(batch(Json::array({source}), 2)).at("facts").at(0);
    check(fact.at("reason_code") == "invalid-request" && fact.at("result_bytes").is_null() &&
              fact.at("selection_ns").is_null() && fact.at("request").is_null() &&
              fact.at("request_fingerprint").is_null(),
          "valid failed fact retained success-only data");
    for (const auto* key : {"route_id", "workspace_id", "source_host_id", "snapshot_id", "preparation_id", "revision",
                            "profile_id", "profile_fingerprint", "route_fingerprint"})
        check(fact.at(key).is_null(), "failed fact fabricated provenance");
    const auto unavailable_quality = null_quality();
    for (const auto& [key, value] : unavailable_quality.items()) {
        (void)value;
        check(fact.at(key).is_null(), "failed fact fabricated cohort metric");
    }
}

void capture_budget_and_batch_bounds() {
    auto source = full_fact(1);
    Json request{{"query", ""}};
    request["query"] = std::string(8192 - request.dump().size(), 'x');
    check(request.dump().size() == 8192, "native request budget fixture is not exact");
    source["request"] = request;
    const auto within_budget = journal::normalize_batch(batch(Json::array({source}), 1));
    check(within_budget.at("facts").at(0).at("request") == request, "exact 8192-byte capture was rejected");
    source["request"]["query"] = source["request"]["query"].get<std::string>() + "x";
    expect_invalid([&] { static_cast<void>(journal::normalize_batch(batch(Json::array({source}), 1))); });

    Json records = Json::array();
    for (std::uint64_t sequence = 1; sequence <= 257; ++sequence) {
        auto row = full_fact(sequence);
        row["request"] = nullptr;
        row["query_capture"] = "disabled";
        records.push_back(std::move(row));
    }
    expect_invalid([&] { static_cast<void>(journal::normalize_batch(batch(std::move(records), 257))); });
}

void malformed_observations_fail_closed() {
    const auto base = batch(Json::array({full_fact(1)}), 1);
    auto expect = [&](const auto& edit) {
        auto candidate = base;
        edit(candidate);
        expect_invalid([&] { static_cast<void>(journal::normalize_batch(candidate)); });
    };
    expect([](Json& value) { value["instance_id"] = "not-a-digest"; });
    expect([](Json& value) { value["source_host_id"] = "InvalidHost"; });
    expect([](Json& value) { value["latest_sequence"] = 0; });
    expect([](Json& value) { value.erase("producer_epoch"); });
    expect([](Json& value) { value["producer_epoch"] = 0; });
    expect([](Json& value) { value["producer_epoch"] = -1; });
    expect([](Json& value) { value["producer_epoch"] = true; });
    expect([](Json& value) { value["producer_epoch"] = 2.5; });
    expect([](Json& value) { value["producer_epoch"] = std::uint64_t{INT64_MAX} + 1; });
    expect([](Json& value) { value["records"][0]["sequence"] = 0; });
    expect([](Json& value) { value["records"][0]["status"] = 200; value["records"][0]["reason_code"] = "wrong"; });
    expect([](Json& value) { value["records"][0]["elapsed_ns"] = 14; });
    expect([](Json& value) { value["records"][0]["last_phase"] = "selection"; value["records"][0]["phase_ns"]["selection"] = nullptr; });
    expect([](Json& value) { value["records"][0]["context"]["profile_fingerprint"] = "0123456789abcdef"; });
    expect([](Json& value) { value["records"][0]["context"]["route_fingerprint"] = "fnv1a64:0123456789abcdef"; });
    expect([](Json& value) { value["records"][0]["query_capture"] = "disabled"; value["records"][0]["request"] = Json::object(); });
    expect([](Json& value) { value["records"][0]["request"] = Json::array(); });
}

void scoped_mode_and_epoch_provenance() {
    auto source = full_fact(1);
    source["operation"] = "brief";
    source["quality"]["retrieval_mode"] = "identifier-scoped";
    const auto first = journal::normalize_batch(batch(Json::array({source}), 1));
    check(first.at("producer_epoch") == 19 && first.at("facts")[0].at("producer_epoch") == 19 &&
              first.at("facts")[0].at("retrieval_mode") == "identifier-scoped",
          "identifier scope or registered epoch was lost in journal normalization");
    auto later = batch(Json::array({source}), 1);
    later["producer_epoch"] = 20;
    const auto normalized = journal::normalize_batch(later);
    check(normalized.at("facts")[0].at("producer_epoch") == 20 &&
              normalized.at("facts")[0].at("record_hash") == first.at("facts")[0].at("record_hash"),
          "transport epoch rewrote the historical payload hash");
    source["quality"]["retrieval_mode"] = "invented-scope";
    expect_invalid([&] { static_cast<void>(journal::normalize_batch(batch(Json::array({source}), 1))); });
    const Json provenance{{"instance_id", std::string(kInstance)}, {"source_host_id", "fixture-host"},
                          {"server_version", "2.0.0-test"}, {"core_version", "core-test"}};
    check(journal::normalize_producer(provenance) == provenance, "registration provenance was rewritten");
    auto malformed = provenance;
    malformed["source_host_id"] = "InvalidHost";
    expect_invalid([&] { static_cast<void>(journal::normalize_producer(malformed)); });
}

void jsonb_omission_preserves_metadata() {
    auto source = full_fact(1);
    source["request"] = {{"task", {{"objective", std::string("a\0b", 3)}}}};
    const auto omitted = journal::normalize_batch(batch(Json::array({source}), 1)).at("facts")[0];
    check(omitted.at("query_capture") == "capture-failed" && omitted.at("request").is_null() &&
              omitted.at("request_fingerprint").is_null() && omitted.at("status") == 200,
          "JSONB-incompatible text blocked or changed the source outcome");
    source["request"] = {{"query", "literal \\u0000 text"}};
    const auto literal = journal::normalize_batch(batch(Json::array({source}), 1)).at("facts")[0];
    check(literal.at("query_capture") == "captured" && literal.at("request") == source.at("request"),
          "literal escape text was incorrectly omitted");
    source["request"] = {{std::string("a\0b", 3), Json::array({"value"})}};
    const auto key = journal::normalize_batch(batch(Json::array({source}), 1)).at("facts")[0];
    check(key.at("query_capture") == "capture-failed" && key.at("request").is_null(), "JSONB key restriction was missed");
}
void attributed_facts_are_domain_bound() {
    auto source = full_fact();
    const auto normalize = [&](const Json& item) { return journal::normalize_batch(batch(Json::array({item}))).at("facts")[0]; };
    const auto legacy = normalize(source);
    source["attribution"] = {{"principal_id", "alice"}, {"tenant_id", "personal-a"}, {"workspace_id", "primary-workspace"}};
    const auto a = normalize(source);
    check(a.at("scope_principal_id") == "alice" && a.at("scope_tenant_id") == "personal-a" &&
          a.at("scope_workspace_id") == "primary-workspace" && a.at("request") == legacy.at("request") &&
          a.at("request_fingerprint") != legacy.at("request_fingerprint") && a.at("record_hash") != legacy.at("record_hash"),
          "personal fact lost its domain or changed its captured body");
    source["attribution"]["principal_id"] = "bob";
    const auto b = normalize(source);
    check(a.at("record_hash") != b.at("record_hash") && a.at("request_fingerprint") != b.at("request_fingerprint"),
          "different principals shared exact-request identity");
    source["attribution"]["workspace_id"] = "other";
    expect_invalid([&] { static_cast<void>(normalize(source)); });
    source["selection"] = nullptr;
    source["status"] = 400;
    source["reason_code"] = "invalid-request";
    const auto other = normalize(source);
    check(other.at("scope_workspace_id") == "other" && other.at("request_fingerprint") != b.at("request_fingerprint"),
          "failed request lost its workspace or shared a foreign query identity");
    source["attribution"].erase("principal_id");
    expect_invalid([&] { static_cast<void>(normalize(source)); });
    source["attribution"] = nullptr;
    check(!normalize(source).contains("scope_principal_id"), "legacy event received inferred attribution");
}
} // namespace

int main() {
    try {
        neutral_full_fact_is_canonical();
        null_error_fact_is_valid_and_unattributed();
        capture_budget_and_batch_bounds();
        malformed_observations_fail_closed();
        scoped_mode_and_epoch_provenance();
        jsonb_omission_preserves_metadata();
        attributed_facts_are_domain_bound();
        std::cout << "retrieval journal: 7 deterministic native test groups passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
