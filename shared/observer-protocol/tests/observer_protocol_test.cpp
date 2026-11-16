#include "shin/observer_protocol.h"

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

} // namespace

int main() {
    if (shin::observer_protocol::sha256_hex("abc") !=
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") {
        std::cerr << "SHA-256 vector failed\n";
        return 1;
    }
    try {
        shin::observer_protocol::verify_observation(nullptr);
        auto diagnostic_observation = observation();
        diagnostic_observation["diagnostic"] = {{"schema_version", "shin.source-observation-diagnostic.v1"},
            {"stage", "source-inventory"}, {"operation", "read-source-content"}, {"category", "access-denied"}};
        shin::observer_protocol::verify_observation(diagnostic_observation);
        auto resource_observation = diagnostic_observation;
        resource_observation["diagnostic"].update({{"category", "resource-exhausted"},
            {"resource", "process-file-descriptors"}, {"native_error", 24},
            {"descriptor_soft_limit", std::uint64_t{256}}});
        shin::observer_protocol::verify_observation(resource_observation);
        for (const auto& change : Json::array({{{"resource", "/private/path"}},
            {{"native_error", "EMFILE"}}, {{"native_error", -1}}, {{"descriptor_soft_limit", "256"}},
            {{"extra", "arbitrary"}}, {{"category", "access-denied"}}, {{"resource", "memory"}}})) {
            auto unsafe = resource_observation.at("diagnostic"); unsafe.update(change);
            if (!shin::observer_protocol::bounded_observation_diagnostic(unsafe).is_null())
                throw std::runtime_error("unbounded resource diagnostic accepted");
        }
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

    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
