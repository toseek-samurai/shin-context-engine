#include "shin/server/checkpoint.h"
#include "shin/server/config.h"

#include <functional>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>

namespace {
using namespace shin::server;

void check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

CheckpointMutation valid_mutation() {
    return {
        .task_id = "checkpoint-task",
        .thread_id = "checkpoint-thread",
        .request_id = "0123456789abcdef0123456789abcdef",
        .request_hash = std::string(64, 'a'),
        .expected_revision = 0,
        .request_deadline = 1,
    };
}

void rejects(const std::function<void()>& action, const char* message) {
    try {
        action();
    } catch (const ServiceError& error) {
        check(error.status == 400 && error.code == "invalid-request", "invalid checkpoint mutation returned the wrong error");
        return;
    }
    throw std::runtime_error(message);
}

void corrupts(const std::function<void()>& action, const char* private_marker, const char* message) {
    try {
        action();
    } catch (const ServiceError& error) {
        check(error.status == 500 && error.code == "database-corrupt", "invalid public checkpoint record returned the wrong error");
        check(std::string(error.what()).find(private_marker) == std::string::npos,
              "public checkpoint validation leaked an internal provenance value");
        return;
    }
    throw std::runtime_error(message);
}

void mutation_bounds_fail_closed() {
    check(kCheckpointMaximumBytes == 262144 && kCheckpointMaximumSlots == 1024 &&
              kCheckpointMaximumOwnerSlots == 128 && kCheckpointRetrySeconds == 300,
          "checkpoint contract constants changed");
    validate_checkpoint_mutation(valid_mutation());

    for (const auto& task : {std::string(), std::string(129, 'a'), std::string("line\nbreak")}) {
        auto mutation = valid_mutation();
        mutation.task_id = task;
        rejects([&] { validate_checkpoint_mutation(mutation); }, "invalid task id was accepted");
    }
    for (const auto& thread : {std::string(), std::string(129, 'a'), std::string("tab\tvalue")}) {
        auto mutation = valid_mutation();
        mutation.thread_id = thread;
        rejects([&] { validate_checkpoint_mutation(mutation); }, "invalid thread id was accepted");
    }
    for (const auto& request_id : {std::string(), std::string(31, 'a'), std::string(33, 'a'),
                                   std::string("0123456789abcdef0123456789abcdeg"),
                                   std::string("0123456789abcdef0123456789ABCDEf")}) {
        auto mutation = valid_mutation();
        mutation.request_id = request_id;
        rejects([&] { validate_checkpoint_mutation(mutation); }, "invalid request id was accepted");
    }
    for (const auto& request_hash : {std::string(), std::string(63, 'a'), std::string(65, 'a'),
                                     std::string(64, 'A'), std::string(63, 'a') + "g"}) {
        auto mutation = valid_mutation();
        mutation.request_hash = request_hash;
        rejects([&] { validate_checkpoint_mutation(mutation); }, "invalid request hash was accepted");
    }
    for (const auto deadline : {std::int64_t{-1}, std::int64_t{253402300800LL}}) {
        auto mutation = valid_mutation();
        mutation.request_deadline = deadline;
        rejects([&] { validate_checkpoint_mutation(mutation); }, "out-of-range request deadline was accepted");
    }
    auto overflowing_revision = valid_mutation();
    overflowing_revision.expected_revision = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + 1U;
    rejects([&] { validate_checkpoint_mutation(overflowing_revision); }, "out-of-range expected revision was accepted");
}

void receipt_origins_bind_exact_owner_scope_and_body() {
    const Json binding{{"root", "/private/source"}, {"profile", "fixture"},
        {"profile_config_hash", "fnv1a64:0123456789abcdef"}, {"task_id", "task"}, {"thread_id", "thread"}};
    const Json receipt{{"producer", "owner"}, {"scope", binding}, {"receipt_id", "test"}, {"result", "pass"}};
    const auto hash = digest_hex(sha256(receipt.dump()));
    const Json document{{"binding", binding}, {"receipts", Json::array({receipt})}};
    const Json origins = Json::array({hash});
    check(checkpoint_receipt_origins_valid(document, "owner", origins), "exact origin was rejected");
    check(!checkpoint_receipt_origins_valid(document, "other", origins), "origin crossed owner");
    check(!checkpoint_receipt_origins_valid(document, "owner", Json::array({hash, hash})), "duplicate origin accepted");
    check(!checkpoint_receipt_origins_valid(document, "owner", Json::array({std::string(64, 'A')})), "invalid digest accepted");
    check(!checkpoint_receipt_origins_valid(document, "owner", Json::array({std::string(64, '0')})), "orphan origin accepted");
    for (const auto* field : {"root", "profile", "profile_config_hash", "task_id", "thread_id"}) {
        auto changed = document;
        changed["binding"][field] = "other";
        check(!checkpoint_receipt_origins_valid(changed, "owner", origins), "origin crossed scope");
    }
    auto changed = document;
    changed["receipts"][0]["result"] = "fail";
    check(!checkpoint_receipt_origins_valid(changed, "owner", origins), "receipt id reuse granted origin");
    check(checkpoint_receipt_origins_valid(Json(), "owner", Json::array()), "empty policy is not neutral");
}

void public_record_omits_canonical_payload_and_replay_secrets() {
    const Json internal = {
        {"schema_version", "shin.task-checkpoint.v1"},
        {"project_id", "visible"},
        {"owner_id", "owner"},
        {"slot_id", "checkpoint-slot"},
        {"task_id", "checkpoint-task"},
        {"thread_id", "checkpoint-thread"},
        {"revision", 7},
        {"state", "active"},
        {"captured_at", 1234},
        {"ttl_seconds", 3600},
        {"checkpoint_id", "fnv1a64:0123456789abcdef"},
        {"updated_at", 1235},
        {"retry_until", 1300},
        {"provenance", {
            {"project_id", "visible"}, {"workspace_id", "primary"}, {"source_host_id", "local-dev"},
            {"route_id", "visible-retrieval"}, {"profile_id", "generic-docs"},
            {"selector", {{"kind", "route"}, {"id", "visible-retrieval"}}},
            {"snapshot_id", "fnv1a64:0123456789abcdef"}, {"preparation_id", 7}, {"revision", 3},
        }},
        {"checkpoint_text", "private canonical checkpoint"},
        {"receipt_origins", Json::array({std::string(64, 'c')})},
        {"request_hash", std::string(64, 'a')},
        {"request_id", "0123456789abcdef0123456789abcdef"},
        {"request_deadline", 1300},
        {"principal_id", "private-owner"},
        {"replayed", true},
    };
    const Json public_record = checkpoint_public_record(internal);
    check(public_record.is_object(), "checkpoint public record is not an object");
    const std::set<std::string> expected_fields{
        "schema_version", "project_id", "owner_id", "slot_id", "state", "revision", "task_id", "thread_id",
        "provenance", "captured_at", "ttl_seconds", "checkpoint_id", "updated_at", "retry_until",
    };
    std::set<std::string> actual_fields;
    for (const auto& [key, value] : public_record.items()) {
        (void)value;
        actual_fields.insert(key);
    }
    check(actual_fields == expected_fields, "checkpoint public projection whitelist changed");
    check(public_record.value("schema_version", "") == "shin.task-checkpoint.v1" &&
              public_record.value("task_id", "") == "checkpoint-task" &&
              public_record.value("thread_id", "") == "checkpoint-thread" &&
              public_record.value("revision", 0) == 7 && public_record.value("state", "") == "active",
          "checkpoint public projection lost public metadata");
    for (const auto* private_key : {"checkpoint_text", "request_hash", "request_id", "request_deadline", "principal_id", "replayed"}) {
        check(!public_record.contains(private_key), "checkpoint public projection leaked private storage metadata");
    }
    check(public_record.dump().find("private canonical checkpoint") == std::string::npos &&
              public_record.dump().find(std::string(64, 'a')) == std::string::npos,
          "checkpoint public projection leaked canonical content");

    for (const auto& [field, value, marker] : std::initializer_list<std::tuple<const char*, Json, const char*>>{
             {"root", "/private/checkpoint-root", "/private/checkpoint-root"},
             {"request_hash", std::string(64, 'b'), "bbbbbbbb"},
             {"workspace_id", 7, "workspace_id"},
         }) {
        auto malformed = internal;
        malformed["provenance"][field] = value;
        corrupts([&] { (void)checkpoint_public_record(malformed); }, marker,
                 "malformed nested checkpoint provenance was projected");
    }
}

} // namespace

int main() {
    try {
        mutation_bounds_fail_closed();
        receipt_origins_bind_exact_owner_scope_and_body();
        public_record_omits_canonical_payload_and_replay_secrets();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
