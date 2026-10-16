#include "shin/observer_protocol.h"

#include <array>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>

namespace {
using shin::observer_protocol::Json;

Json observation() {
    return {{"status", "error"}, {"reason_code", "observation-failed"}, {"started_at_ms", 1}, {"snapshot_id", nullptr},
        {"worktree_id", nullptr}, {"snapshot_current", nullptr}, {"profile_config_hash", nullptr}, {"source_counts", nullptr},
        {"changes", nullptr}, {"details_omitted", 0}, {"policy", "periodic-profile-content-comparison"},
        {"verification_level", "local-source-observed"}, {"source_mutated", false}, {"index_mutated", false},
        {"git_required", false}, {"readiness_assessed", false}, {"observed_at_ms", 2}, {"duration_ms", 0.0}};
}

Json submission(std::int64_t sequence = 1) {
    Json payload{{"observer_id", "observer"}, {"source_host_id", "source-host"}, {"project_id", "project"},
        {"workspace_id", "workspace"}, {"profile", "profile"}, {"workspace_binding_digest", std::string(64, 'a')},
        {"observer_epoch", std::string(32, 'b')}, {"observer_sequence", sequence}, {"observation", observation()},
        {"verification_level", "producer-observed"}, {"shared_publication_mutated", false}};
    const std::string stream(32, 'c');
    return {{"schema_version", "shin.observer-submission.v1"}, {"stream_id", stream}, {"sequence", sequence},
        {"submission_id", shin::observer_protocol::sha256_hex(stream + ":" + std::to_string(sequence))},
        {"payload_sha256", shin::observer_protocol::sha256_hex(payload.dump())}, {"payload", std::move(payload)}};
}

Json acknowledgement(const Json& value, const std::string& server_id = std::string(32, 'd')) {
    return {{"schema_version", "shin.observer-ack.v1"}, {"server_id", server_id}, {"stream_id", value.at("stream_id")},
        {"sequence", value.at("sequence")}, {"submission_id", value.at("submission_id")},
        {"payload_sha256", value.at("payload_sha256")}, {"project_id", value.at("payload").at("project_id")},
        {"workspace_id", value.at("payload").at("workspace_id")}, {"received_at", "2026-09-06T12:34:56.123456Z"},
        {"verification_level", "producer-observed"}, {"server_source_verified", false}, {"shared_publication_mutated", false}};
}

Json reconciliation_request(const Json& receipt, const Json& next_submission = nullptr) {
    const auto acknowledged_sequence = receipt.is_null() ? 0 : receipt.at("sequence").get<std::int64_t>();
    const auto accepted_sequence = next_submission.is_null() ? acknowledged_sequence : next_submission.at("sequence").get<std::int64_t>();
    return {{"schema_version", "shin.observer-reconcile-request.v1"}, {"server_id", std::string(32, 'd')},
        {"stream_id", std::string(32, 'c')}, {"acknowledged_sequence", acknowledged_sequence},
        {"accepted_sequence", accepted_sequence}, {"last_receipt", receipt}, {"next_submission", next_submission}};
}

Json confirmation(const Json& receipt) {
    return {{"schema_version", "shin.observer-confirmation.v1"}, {"server_id", receipt.at("server_id")},
        {"stream_id", receipt.at("stream_id")}, {"confirmed_sequence", receipt.at("sequence")}, {"retired_sequence", 0},
        {"producer_durability_verified", false}, {"shared_publication_mutated", false}};
}

Json reconciliation_result(const Json& request) {
    const auto& acknowledged_sequence = request.at("acknowledged_sequence");
    return {{"schema_version", "shin.observer-reconcile-result.v1"}, {"server_id", request.at("server_id")},
        {"stream_id", request.at("stream_id")}, {"state", "caught-up"},
        {"local_acknowledged_sequence", acknowledged_sequence}, {"local_accepted_sequence", request.at("accepted_sequence")},
        {"server_accepted_sequence", acknowledged_sequence}, {"confirmed_sequence", acknowledged_sequence},
        {"retired_sequence", 0}, {"retained_submissions", acknowledged_sequence}, {"retained_bytes", 0},
        {"confirmation_required", false}, {"next_action", "none"}, {"resume_allowed", true},
        {"observer_state_mutated", false}, {"shared_publication_mutated", false}, {"source_verified", false},
        {"local_state_verified", false}, {"verification_level", "service-observed"}};
}

bool rejected(const Json& value) {
    try {
        shin::observer_protocol::verify_submission(value);
    } catch (const shin::observer_protocol::ValidationError&) {
        return true;
    }
    return false;
}

bool receipt_rejected(const Json& value) {
    try {
        shin::observer_protocol::verify_acknowledgement_shape(value);
    } catch (const shin::observer_protocol::ValidationError&) {
        return true;
    }
    return false;
}

bool acknowledgement_rejected(const Json& receipt, const Json& value, const std::string& server_id) {
    try {
        shin::observer_protocol::verify_acknowledgement(receipt, value, server_id);
    } catch (const shin::observer_protocol::ValidationError&) {
        return true;
    }
    return false;
}

bool reconciliation_rejected(const Json& request) {
    try {
        shin::observer_protocol::verify_reconciliation_request(request);
    } catch (const shin::observer_protocol::ValidationError&) {
        return true;
    }
    return false;
}

bool reconciliation_result_rejected(const Json& result, const Json& request) {
    try {
        shin::observer_protocol::verify_reconciliation_result(result, request);
    } catch (const shin::observer_protocol::ValidationError&) {
        return true;
    }
    return false;
}

bool confirmation_rejected(const Json& response, const Json& receipt) {
    try {
        shin::observer_protocol::verify_confirmation(response, receipt);
    } catch (const shin::observer_protocol::ValidationError&) {
        return true;
    }
    return false;
}

void check_reconciliation_retention_boundaries() {
    struct Case {
        const char* name;
        std::int64_t acknowledged;
        std::int64_t issued;
        std::int64_t accepted;
        std::int64_t confirmed;
        std::int64_t retired;
        const char* state;
        bool resume;
        bool valid;
    };
    const std::array cases{
        Case{"empty", 0, 0, 0, 0, 0, "caught-up", true, true},
        Case{"first-send", 0, 1, 0, 0, 0, "send-next", true, true},
        Case{"unpruned-replay", 0, 2, 2, 2, 0, "replay-head", true, true},
        Case{"retired-head", 0, 2, 2, 2, 1, "replay-head", true, false},
        Case{"retired-anchor-retained-head", 1, 3, 3, 3, 1, "replay-head", true, false},
        Case{"retired-anchor-and-head", 1, 3, 3, 3, 2, "replay-head", true, false},
        Case{"retained-anchor", 2, 3, 3, 3, 1, "replay-head", true, true},
        Case{"longer-local-suffix", 2, 4, 3, 3, 1, "replay-head", true, true},
        Case{"server-ahead-is-not-replay", 1, 3, 4, 4, 0, "replay-head", true, false},
        Case{"caught-up-after-pruning", 2, 2, 2, 2, 1, "caught-up", true, true},
        Case{"send-after-pruning", 2, 3, 2, 2, 1, "send-next", true, true},
        Case{"retired-blocked", 0, 2, 2, 2, 1, "history-retired", false, true},
        Case{"server-ahead-blocked", 0, 1, 2, 2, 0, "server-ahead", false, true},
    };
    for (const auto& item : cases) {
        const auto receipt = item.acknowledged == 0 ? Json(nullptr) : acknowledgement(submission(item.acknowledged));
        const auto next = item.issued == item.acknowledged ? Json(nullptr) : submission(item.acknowledged + 1);
        auto request = reconciliation_request(receipt, next);
        request["accepted_sequence"] = item.issued;
        shin::observer_protocol::verify_reconciliation_request(request);
        auto result = reconciliation_result(request);
        result["server_accepted_sequence"] = item.accepted;
        result["confirmed_sequence"] = item.confirmed;
        result["retired_sequence"] = item.retired;
        result["retained_submissions"] = item.accepted - item.retired;
        result["state"] = item.state;
        result["resume_allowed"] = item.resume;
        result["confirmation_required"] = item.resume && item.acknowledged > item.confirmed;
        result["next_action"] = !item.resume ? "operator-review" : std::string_view(item.state) == "caught-up" ? "none" : item.state;
        if (reconciliation_result_rejected(result, request) == item.valid)
            throw std::runtime_error(std::string("reconciliation retention boundary: ") + item.name);
    }
}
} // namespace

int main() {
    if (shin::observer_protocol::sha256_hex("abc") !=
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") {
        std::cerr << "SHA-256 vector failed\n";
        return 1;
    }
    try {
        check_reconciliation_retention_boundaries();
        shin::observer_protocol::verify_observation(nullptr);
        auto diagnostic_observation = observation();
        diagnostic_observation["diagnostic"] = {{"schema_version", "shin.source-observation-diagnostic.v1"},
            {"stage", "source-inventory"}, {"operation", "read-source-content"}, {"category", "access-denied"}};
        shin::observer_protocol::verify_observation(diagnostic_observation);
        for (const auto* key : {"stage", "operation", "category", "extra"}) {
            auto unsafe = diagnostic_observation;
            unsafe["diagnostic"][key] = "/private/source/token";
            bool refused = false;
            try { shin::observer_protocol::verify_observation(unsafe); }
            catch (const shin::observer_protocol::ValidationError&) { refused = true; }
            if (!refused) throw std::runtime_error("unbounded observation diagnostic accepted");
        }

        auto invalid_observation = observation();
        invalid_observation["status"] = "current";
        bool invalid_observation_rejected = false;
        try {
            shin::observer_protocol::verify_observation(invalid_observation);
        } catch (const shin::observer_protocol::ValidationError&) {
            invalid_observation_rejected = true;
        }
        if (!invalid_observation_rejected) throw std::runtime_error("inconsistent observation was accepted");
        const auto valid = submission();
        shin::observer_protocol::verify_submission(valid);
        const std::string server_id(32, 'd');
        const auto receipt = acknowledgement(valid, server_id);
        shin::observer_protocol::verify_acknowledgement_shape(receipt);
        shin::observer_protocol::verify_acknowledgement(receipt, valid, server_id);
        const auto caught_up = reconciliation_request(receipt);
        shin::observer_protocol::verify_reconciliation_request(caught_up);
        const auto caught_up_result = reconciliation_result(caught_up);
        shin::observer_protocol::verify_reconciliation_result(caught_up_result, caught_up);
        auto server_identity_mismatch = caught_up_result;
        server_identity_mismatch["server_id"] = std::string(32, 'e');
        server_identity_mismatch["state"] = "server-identity-mismatch";
        server_identity_mismatch["next_action"] = "operator-review";
        server_identity_mismatch["resume_allowed"] = false;
        shin::observer_protocol::verify_reconciliation_result(server_identity_mismatch, caught_up);
        const auto next_submission = submission(2);
        shin::observer_protocol::verify_reconciliation_request(reconciliation_request(receipt, next_submission));
        const auto valid_confirmation = confirmation(receipt);
        shin::observer_protocol::verify_confirmation(valid_confirmation, receipt);

        auto extra_field = valid;
        extra_field["unexpected"] = true;
        if (!rejected(extra_field)) throw std::runtime_error("extra submission field was accepted");

        auto tampered_digest = valid;
        tampered_digest["payload_sha256"] = std::string(64, '0');
        if (!rejected(tampered_digest)) throw std::runtime_error("tampered digest was accepted");

        auto null_observation = valid;
        null_observation["payload"]["observation"] = nullptr;
        null_observation["payload_sha256"] = shin::observer_protocol::sha256_hex(null_observation["payload"].dump());
        if (!rejected(null_observation)) throw std::runtime_error("null observation was accepted");

        auto invalid_sequence = valid;
        invalid_sequence["sequence"] = 0;
        if (!rejected(invalid_sequence)) throw std::runtime_error("zero sequence was accepted");

        auto false_authority = valid;
        false_authority["payload"]["verification_level"] = "server-source-verified";
        false_authority["payload_sha256"] = shin::observer_protocol::sha256_hex(false_authority["payload"].dump());
        if (!rejected(false_authority)) throw std::runtime_error("false authority was accepted");

        auto oversized = valid;
        oversized["payload"]["observer_id"] = std::string(8192, 'a');
        oversized["payload_sha256"] = shin::observer_protocol::sha256_hex(oversized["payload"].dump());
        if (!rejected(oversized)) throw std::runtime_error("oversized submission was accepted");

        auto extra_receipt_field = receipt;
        extra_receipt_field["unexpected"] = true;
        if (!receipt_rejected(extra_receipt_field)) throw std::runtime_error("extra acknowledgement field was accepted");

        auto invalid_timestamp = receipt;
        invalid_timestamp["received_at"] = "2026-02-29T12:34:56.123456Z";
        if (!receipt_rejected(invalid_timestamp)) throw std::runtime_error("invalid acknowledgement timestamp was accepted");

        auto invalid_server_id = receipt;
        invalid_server_id["server_id"] = std::string(32, 'D');
        if (!receipt_rejected(invalid_server_id)) throw std::runtime_error("non-canonical server identity was accepted");

        for (const auto* key : {"verification_level", "server_source_verified", "shared_publication_mutated"}) {
            auto elevated = receipt;
            if (std::string_view(key) == "verification_level") elevated[key] = "server-source-verified";
            else elevated[key] = true;
            if (!receipt_rejected(elevated)) throw std::runtime_error("elevated acknowledgement provenance was accepted");
        }

        for (const auto* key : {"server_id", "stream_id", "sequence", "submission_id", "payload_sha256", "project_id", "workspace_id"}) {
            auto altered = receipt;
            if (std::string_view(key) == "sequence") altered[key] = 2;
            else if (std::string_view(key) == "server_id" || std::string_view(key) == "stream_id") altered[key] = std::string(32, 'e');
            else if (std::string_view(key) == "submission_id" || std::string_view(key) == "payload_sha256") altered[key] = std::string(64, 'e');
            else altered[key] = "other";
            if (!acknowledgement_rejected(altered, valid, server_id))
                throw std::runtime_error("altered acknowledgement identity was accepted");
        }
        if (!acknowledgement_rejected(receipt, valid, std::string(32, 'e')))
            throw std::runtime_error("unexpected server identity was accepted");
        if (!acknowledgement_rejected(receipt, valid, "server"))
            throw std::runtime_error("malformed expected server identity was accepted");

        auto false_zero_ack = reconciliation_request(nullptr);
        false_zero_ack["last_receipt"] = receipt;
        if (!reconciliation_rejected(false_zero_ack)) throw std::runtime_error("zero acknowledgement receipt was accepted");

        auto missing_next = caught_up;
        missing_next["accepted_sequence"] = 2;
        if (!reconciliation_rejected(missing_next)) throw std::runtime_error("missing next submission was accepted");

        auto reused_next = reconciliation_request(receipt, valid);
        if (!reconciliation_rejected(reused_next)) throw std::runtime_error("reused next submission was accepted");

        auto foreign_identity = caught_up;
        foreign_identity["last_receipt"]["server_id"] = std::string(32, 'e');
        if (!reconciliation_rejected(foreign_identity)) throw std::runtime_error("foreign receipt identity was accepted");

        auto foreign_submission_identity = reconciliation_request(receipt, next_submission);
        foreign_submission_identity["next_submission"]["payload"]["project_id"] = "other";
        foreign_submission_identity["next_submission"]["payload_sha256"] =
            shin::observer_protocol::sha256_hex(foreign_submission_identity["next_submission"]["payload"].dump());
        if (!reconciliation_rejected(foreign_submission_identity))
            throw std::runtime_error("foreign submission identity was accepted");

        auto malformed_counters = caught_up;
        malformed_counters["acknowledged_sequence"] = -1;
        if (!reconciliation_rejected(malformed_counters)) throw std::runtime_error("negative acknowledgement was accepted");
        malformed_counters = caught_up;
        malformed_counters["accepted_sequence"] = 0;
        if (!reconciliation_rejected(malformed_counters)) throw std::runtime_error("regressed accepted sequence was accepted");

        auto wrong_confirmation_id = valid_confirmation;
        wrong_confirmation_id["server_id"] = std::string(32, 'e');
        if (!confirmation_rejected(wrong_confirmation_id, receipt)) throw std::runtime_error("foreign confirmation identity was accepted");

        auto retired_receipt = valid_confirmation;
        retired_receipt["retired_sequence"] = receipt.at("sequence");
        if (!confirmation_rejected(retired_receipt, receipt)) throw std::runtime_error("retired confirmation receipt was accepted");

        auto future_caught_up = caught_up_result;
        future_caught_up["server_accepted_sequence"] = 2;
        future_caught_up["confirmed_sequence"] = 2;
        future_caught_up["retained_submissions"] = 2;
        if (!reconciliation_result_rejected(future_caught_up, caught_up))
            throw std::runtime_error("future caught-up result was accepted");

        auto unknown_state = caught_up_result;
        unknown_state["state"] = "future-state";
        unknown_state["next_action"] = "future-state";
        if (!reconciliation_result_rejected(unknown_state, caught_up))
            throw std::runtime_error("unknown reconciliation state was accepted");

        for (const auto* key : {"observer_state_mutated", "shared_publication_mutated", "source_verified", "local_state_verified"}) {
            auto elevated = caught_up_result;
            elevated[key] = true;
            if (!reconciliation_result_rejected(elevated, caught_up))
                throw std::runtime_error("non-false reconciliation flag was accepted");
        }

        auto wrong_local_state = caught_up_result;
        wrong_local_state["local_acknowledged_sequence"] = 0;
        if (!reconciliation_result_rejected(wrong_local_state, caught_up))
            throw std::runtime_error("wrong local reconciliation state was accepted");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
