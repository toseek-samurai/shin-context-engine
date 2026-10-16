#include "shin/observer_protocol.h"

#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#else
#include <openssl/evp.h>
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <set>
#include <string>

namespace shin::observer_protocol {
namespace {
constexpr std::int64_t kMaximumSequence = std::numeric_limits<std::int64_t>::max() - 1;

[[noreturn]] void invalid(const char* message) { throw ValidationError(message); }

bool exact_fields(const Json& object, const std::set<std::string>& keys) {
    return object.is_object() && object.size() == keys.size() &&
        std::all_of(keys.begin(), keys.end(), [&](const auto& key) { return object.contains(key); });
}

bool nonnegative(const Json& value) {
    return value.is_number_integer() && value >= 0 && value <= std::numeric_limits<std::int64_t>::max();
}

bool false_value(const Json& value) { return value.is_boolean() && value == false; }

bool fingerprint(const Json& value) {
    if (!value.is_string()) return false;
    const auto& text = value.get_ref<const std::string&>();
    return text.size() == 24 && text.starts_with("fnv1a64:") &&
        text.find_first_not_of("0123456789abcdef", 8) == std::string::npos;
}

bool counts(const Json& value, const std::set<std::string>& keys) {
    return exact_fields(value, keys) && std::all_of(value.begin(), value.end(), nonnegative);
}

bool hex(const Json& value, std::size_t length) {
    if (!value.is_string()) return false;
    const auto& text = value.get_ref<const std::string&>();
    return text.size() == length && text.find_first_not_of("0123456789abcdef") == std::string::npos;
}

bool slug(const Json& value) {
    if (!value.is_string()) return false;
    const auto& text = value.get_ref<const std::string&>();
    return !text.empty() && text.size() <= 63 && text.front() >= 'a' && text.front() <= 'z' &&
        text.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789-") == std::string::npos;
}

bool submission_sequence(const Json& value) {
    return value.is_number_integer() && value > 0 && value <= kMaximumSequence;
}

bool canonical_utc_timestamp(const Json& value) {
    if (!value.is_string()) return false;
    const auto& text = value.get_ref<const std::string&>();
    if (text.size() != 27 || text[4] != '-' || text[7] != '-' || text[10] != 'T' || text[13] != ':' ||
        text[16] != ':' || text[19] != '.' || text[26] != 'Z')
        return false;
    for (const auto index : {0U, 1U, 2U, 3U, 5U, 6U, 8U, 9U, 11U, 12U, 14U, 15U, 17U, 18U, 20U, 21U, 22U, 23U, 24U, 25U})
        if (text[index] < '0' || text[index] > '9') return false;
    const auto number = [&](std::size_t offset, std::size_t width) {
        int result = 0;
        for (std::size_t index = offset; index < offset + width; ++index) result = result * 10 + text[index] - '0';
        return result;
    };
    const auto year = number(0, 4);
    const auto month = number(5, 2);
    const auto day = number(8, 2);
    const std::chrono::year_month_day date{std::chrono::year{year}, std::chrono::month{static_cast<unsigned>(month)},
                                           std::chrono::day{static_cast<unsigned>(day)}};
    return year != 0 && date.ok() && number(11, 2) <= 23 && number(14, 2) <= 59 && number(17, 2) <= 59;
}

void verify_observation_fields(const Json& value) {
    if (value.is_null()) return;
    std::set<std::string> keys{"status", "reason_code", "started_at_ms", "snapshot_id", "worktree_id",
        "snapshot_current", "profile_config_hash", "source_counts", "changes", "details_omitted", "policy",
        "verification_level", "source_mutated", "index_mutated", "git_required", "readiness_assessed", "observed_at_ms", "duration_ms"};
    if (value.is_object() && value.contains("diagnostic")) {
        keys.insert("diagnostic");
        if (value.value("status", Json()) != "error" || bounded_observation_diagnostic(value.at("diagnostic")).is_null())
            invalid("Stored observation diagnostic is invalid.");
    }
    if (!exact_fields(value, keys)) invalid("Stored observation fields are invalid.");
    const std::set<std::string> statuses{"current", "stale", "unpublished", "incomplete", "error"};
    const auto& reason = value.at("reason_code");
    if (!value.at("status").is_string() || !statuses.contains(value.at("status").get<std::string>()) ||
        !nonnegative(value.at("started_at_ms")) || !nonnegative(value.at("observed_at_ms")) ||
        !nonnegative(value.at("details_omitted")) || !value.at("duration_ms").is_number() ||
        !std::isfinite(value.at("duration_ms").get<double>()) || value.at("duration_ms") < 0 ||
        value.at("policy") != "periodic-profile-content-comparison" || value.at("verification_level") != "local-source-observed" ||
        value.at("source_mutated") != false || value.at("index_mutated") != false || value.at("git_required") != false ||
        value.at("readiness_assessed") != false ||
        (!reason.is_null() && (!reason.is_string() || reason.get_ref<const std::string&>().empty() ||
            reason.get_ref<const std::string&>().size() > 128 ||
            reason.get_ref<const std::string&>().find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789-") != std::string::npos)))
        invalid("Stored observation values are invalid.");
    for (const auto* key : {"snapshot_id", "worktree_id", "profile_config_hash"})
        if (!value.at(key).is_null() && !fingerprint(value.at(key))) invalid("Stored fingerprint is invalid.");
    if ((!value.at("snapshot_current").is_null() && !value.at("snapshot_current").is_boolean()) ||
        (!value.at("source_counts").is_null() && !counts(value.at("source_counts"), {"documents", "source_facts", "candidate_files", "capped_files", "skipped_files"})) ||
        (!value.at("changes").is_null() && !counts(value.at("changes"), {"metadata_changed", "documents_added", "documents_removed",
            "documents_changed", "source_facts_added", "source_facts_removed", "source_facts_changed", "total_changes"})))
        invalid("Stored observation counts are invalid.");
    if (value.at("status") == "current" && (value.at("snapshot_current") != true || reason != nullptr ||
        value.at("snapshot_id").is_null() || value.at("worktree_id").is_null() || value.at("profile_config_hash").is_null() ||
        value.at("changes").is_null() || value.at("changes").at("total_changes") != 0))
        invalid("Stored current observation is inconsistent.");
    if (value.at("status") != "current" && (value.at("snapshot_current") == true || reason.is_null()))
        invalid("Stored non-current observation is inconsistent.");
    if (value.at("status") == "error") {
        for (const auto* key : {"snapshot_id", "worktree_id", "snapshot_current", "profile_config_hash", "source_counts", "changes"})
            if (!value.at(key).is_null()) invalid("Failed observation retained unverified metadata.");
        if (value.at("details_omitted") != 0) invalid("Failed observation retained change counts.");
        return;
    }
    const auto& source = value.at("source_counts");
    if (source.is_null() || value.at("worktree_id").is_null() || value.at("profile_config_hash").is_null() ||
        source.at("skipped_files") > source.at("source_facts"))
        invalid("Successful observation is missing source metadata.");
    const bool incomplete = source.at("capped_files") != 0 || source.at("skipped_files") != 0;
    if ((value.at("status") == "incomplete") != incomplete)
        invalid("Source completeness metadata is inconsistent.");
    if ((value.at("status") == "stale" && reason != "worktree-not-current") ||
        (value.at("status") == "unpublished" && reason != "index-unavailable") ||
        (value.at("status") == "incomplete" && reason != "incomplete-source-observation"))
        invalid("Observation reason disagrees with its status.");
    if (value.at("snapshot_id").is_null()) {
        if (!value.at("snapshot_current").is_null() || !value.at("changes").is_null() || value.at("details_omitted") != 0 ||
            (value.at("status") != "unpublished" && value.at("status") != "incomplete"))
            invalid("Unpublished observation claimed a snapshot comparison.");
    } else if (value.at("status") == "unpublished" || !value.at("snapshot_current").is_boolean() || value.at("changes").is_null() ||
               value.at("details_omitted") != value.at("changes").at("total_changes")) {
        invalid("Published observation has invalid comparison metadata.");
    }
    if (value.at("status") == "stale" && value.at("changes").at("total_changes") == 0)
        invalid("Stale observation has no recorded changes.");
}
} // namespace

std::string sha256_hex(std::string_view value) {
#ifdef _WIN32
    std::array<unsigned char, 32> bytes{};
    constexpr unsigned int count = 32;
    if (value.size() > std::numeric_limits<ULONG>::max() ||
        BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0,
            reinterpret_cast<PUCHAR>(const_cast<char*>(value.data())), static_cast<ULONG>(value.size()), bytes.data(), count) != 0)
        throw std::runtime_error("Could not compute SHA-256.");
#else
    std::array<unsigned char, EVP_MAX_MD_SIZE> bytes{};
    unsigned int count = 0;
    if (EVP_Digest(value.data(), value.size(), bytes.data(), &count, EVP_sha256(), nullptr) != 1 || count != 32)
        throw std::runtime_error("Could not compute SHA-256.");
#endif
    std::string result;
    result.reserve(count * 2);
    for (unsigned int i = 0; i < count; ++i) {
        result.push_back("0123456789abcdef"[bytes[i] >> 4]);
        result.push_back("0123456789abcdef"[bytes[i] & 15]);
    }
    return result;
}

void verify_observation(const Json& observation) { verify_observation_fields(observation); }

void verify_submission(const Json& submission) {
    const std::set<std::string> fields{"schema_version", "stream_id", "sequence", "submission_id", "payload_sha256", "payload"};
    if (!exact_fields(submission, fields)) invalid("Submission fields are invalid.");
    const auto& payload = submission.at("payload");
    const std::set<std::string> payload_fields{"observer_id", "source_host_id", "project_id", "workspace_id", "profile",
        "workspace_binding_digest", "observer_epoch", "observer_sequence", "observation", "verification_level", "shared_publication_mutated"};
    if (submission.dump().size() > 8192 || submission.at("schema_version") != "shin.observer-submission.v1" ||
        !hex(submission.at("stream_id"), 32) || !submission_sequence(submission.at("sequence")) ||
        !exact_fields(payload, payload_fields))
        invalid("Submission schema is invalid.");
    for (const auto* key : {"observer_id", "source_host_id", "project_id", "workspace_id", "profile"})
        if (!slug(payload.at(key))) invalid("Submission identity is invalid.");
    if (!hex(payload.at("workspace_binding_digest"), 64) || !hex(payload.at("observer_epoch"), 32) ||
        !submission_sequence(payload.at("observer_sequence")) || payload.at("observation").is_null() ||
        payload.at("verification_level") != "producer-observed" || payload.at("shared_publication_mutated") != false)
        invalid("Submission payload is invalid.");
    const auto sequence = submission.at("sequence").get<std::int64_t>();
    const auto expected_submission_id = sha256_hex(submission.at("stream_id").get<std::string>() + ":" + std::to_string(sequence));
    if (submission.at("submission_id") != expected_submission_id || submission.at("payload_sha256") != sha256_hex(payload.dump()))
        invalid("Submission digest is invalid.");
    verify_observation(payload.at("observation"));
}

void verify_acknowledgement_shape(const Json& receipt) {
    const std::set<std::string> fields{"schema_version", "server_id", "stream_id", "sequence", "submission_id",
        "payload_sha256", "project_id", "workspace_id", "received_at", "verification_level", "server_source_verified",
        "shared_publication_mutated"};
    if (!exact_fields(receipt, fields) || receipt.at("schema_version") != "shin.observer-ack.v1" ||
        !hex(receipt.at("server_id"), 32) || !hex(receipt.at("stream_id"), 32) || !submission_sequence(receipt.at("sequence")) ||
        !hex(receipt.at("submission_id"), 64) || !hex(receipt.at("payload_sha256"), 64) ||
        !slug(receipt.at("project_id")) || !slug(receipt.at("workspace_id")) || !canonical_utc_timestamp(receipt.at("received_at")) ||
        receipt.at("verification_level") != "producer-observed" || receipt.at("server_source_verified") != false ||
        receipt.at("shared_publication_mutated") != false)
        invalid("Acknowledgement schema is invalid.");
}

void verify_acknowledgement(const Json& receipt, const Json& submission, const std::string& server_id) {
    verify_submission(submission);
    verify_acknowledgement_shape(receipt);
    const Json expected_server_id = server_id;
    if (!hex(expected_server_id, 32) || receipt.at("server_id") != server_id ||
        receipt.at("stream_id") != submission.at("stream_id") || receipt.at("sequence") != submission.at("sequence") ||
        receipt.at("submission_id") != submission.at("submission_id") || receipt.at("payload_sha256") != submission.at("payload_sha256") ||
        receipt.at("project_id") != submission.at("payload").at("project_id") ||
        receipt.at("workspace_id") != submission.at("payload").at("workspace_id"))
        invalid("Acknowledgement does not match its submission.");
}

void verify_reconciliation_request(const Json& request) {
    const std::set<std::string> fields{"schema_version", "server_id", "stream_id", "acknowledged_sequence",
        "accepted_sequence", "last_receipt", "next_submission"};
    if (!exact_fields(request, fields) || request.at("schema_version") != "shin.observer-reconcile-request.v1" ||
        !hex(request.at("server_id"), 32) || !hex(request.at("stream_id"), 32) ||
        !nonnegative(request.at("acknowledged_sequence")) || !nonnegative(request.at("accepted_sequence")) ||
        request.at("acknowledged_sequence") > kMaximumSequence || request.at("accepted_sequence") > kMaximumSequence ||
        request.at("accepted_sequence") < request.at("acknowledged_sequence"))
        invalid("Reconciliation request counters are invalid.");

    const auto acknowledged_sequence = request.at("acknowledged_sequence").get<std::int64_t>();
    const auto accepted_sequence = request.at("accepted_sequence").get<std::int64_t>();
    const auto& last_receipt = request.at("last_receipt");
    if (acknowledged_sequence == 0) {
        if (!last_receipt.is_null()) invalid("Zero acknowledgement has a receipt.");
    } else {
        verify_acknowledgement_shape(last_receipt);
        if (last_receipt.at("server_id") != request.at("server_id") || last_receipt.at("stream_id") != request.at("stream_id") ||
            last_receipt.at("sequence") != acknowledged_sequence)
            invalid("Reconciliation receipt does not match its cursor.");
    }

    const auto& next_submission = request.at("next_submission");
    if (accepted_sequence == acknowledged_sequence) {
        if (!next_submission.is_null()) invalid("Caught-up reconciliation has a submission.");
        return;
    }

    verify_submission(next_submission);
    if (next_submission.at("stream_id") != request.at("stream_id") || next_submission.at("sequence") != acknowledged_sequence + 1)
        invalid("Reconciliation submission does not match its cursor.");
    if (!last_receipt.is_null() &&
        (next_submission.at("payload").at("project_id") != last_receipt.at("project_id") ||
         next_submission.at("payload").at("workspace_id") != last_receipt.at("workspace_id")))
        invalid("Reconciliation submission does not match its receipt identity.");
}

void verify_reconciliation_result(const Json& result, const Json& request) {
    verify_reconciliation_request(request);
    const std::set<std::string> fields{"schema_version", "server_id", "stream_id", "state", "local_acknowledged_sequence",
        "local_accepted_sequence", "server_accepted_sequence", "confirmed_sequence", "retired_sequence", "retained_submissions",
        "retained_bytes", "confirmation_required", "next_action", "resume_allowed", "observer_state_mutated",
        "shared_publication_mutated", "source_verified", "local_state_verified", "verification_level"};
    if (!exact_fields(result, fields) || result.at("schema_version") != "shin.observer-reconcile-result.v1" ||
        !hex(result.at("server_id"), 32) || result.at("stream_id") != request.at("stream_id") ||
        !nonnegative(result.at("local_acknowledged_sequence")) || !nonnegative(result.at("local_accepted_sequence")) ||
        !nonnegative(result.at("server_accepted_sequence")) || !nonnegative(result.at("confirmed_sequence")) ||
        !nonnegative(result.at("retired_sequence")) || !nonnegative(result.at("retained_submissions")) ||
        !nonnegative(result.at("retained_bytes")) || result.at("server_accepted_sequence") > kMaximumSequence ||
        result.at("retained_bytes") > 4 * 1024 * 1024 || !result.at("state").is_string() ||
        !result.at("next_action").is_string() || !result.at("confirmation_required").is_boolean() ||
        !result.at("resume_allowed").is_boolean() || !false_value(result.at("observer_state_mutated")) ||
        !false_value(result.at("shared_publication_mutated")) || !false_value(result.at("source_verified")) ||
        !false_value(result.at("local_state_verified")) || result.at("verification_level") != "service-observed")
        invalid("Reconciliation result schema is invalid.");

    if (result.at("local_acknowledged_sequence") != request.at("acknowledged_sequence") ||
        result.at("local_accepted_sequence") != request.at("accepted_sequence"))
        invalid("Reconciliation result local cursors do not match its request.");

    const auto state = result.at("state").get<std::string>();
    const std::set<std::string> resume_states{"caught-up", "send-next", "replay-head"};
    const std::set<std::string> blocked_states{"server-behind", "server-ahead", "history-retired", "receipt-conflict",
        "submission-conflict", "history-gap", "revoked", "server-identity-mismatch"};
    if (!resume_states.contains(state) && !blocked_states.contains(state)) invalid("Reconciliation result state is invalid.");
    if (result.at("server_id") != request.at("server_id") && state != "server-identity-mismatch")
        invalid("Reconciliation result server identity is invalid.");

    const auto server_accepted = result.at("server_accepted_sequence").get<std::int64_t>();
    const auto confirmed = result.at("confirmed_sequence").get<std::int64_t>();
    const auto retired = result.at("retired_sequence").get<std::int64_t>();
    if (retired > confirmed || confirmed > server_accepted || server_accepted - retired > 1024 ||
        result.at("retained_submissions") != server_accepted - retired || (retired != 0 && retired >= confirmed))
        invalid("Reconciliation result server counters are invalid.");

    const bool resume = resume_states.contains(state);
    const auto local_acknowledged = request.at("acknowledged_sequence").get<std::int64_t>();
    const auto local_accepted = request.at("accepted_sequence").get<std::int64_t>();
    if (resume && retired != 0 && local_acknowledged <= retired)
        invalid("Reconciliation cannot resume from retired acknowledgement history.");
    if (result.at("resume_allowed") != resume ||
        result.at("confirmation_required") != (resume && local_acknowledged > confirmed) ||
        result.at("next_action") != (state == "caught-up" ? "none" : resume ? state : "operator-review"))
        invalid("Reconciliation result action is invalid.");

    const bool pending = !request.at("next_submission").is_null();
    const auto next_sequence = pending ? request.at("next_submission").at("sequence").get<std::int64_t>() : 0;
    if ((state == "caught-up" && (pending || local_acknowledged != local_accepted || local_acknowledged != server_accepted)) ||
        (state == "send-next" && (!pending || server_accepted != local_acknowledged)) ||
        (state == "replay-head" && (!pending || server_accepted < next_sequence ||
            server_accepted > local_accepted)))
        invalid("Reconciliation result state does not match its request.");
}

void verify_confirmation(const Json& response, const Json& receipt) {
    verify_acknowledgement_shape(receipt);
    const std::set<std::string> fields{"schema_version", "server_id", "stream_id", "confirmed_sequence", "retired_sequence",
        "producer_durability_verified", "shared_publication_mutated"};
    if (!exact_fields(response, fields) || response.at("schema_version") != "shin.observer-confirmation.v1" ||
        !hex(response.at("server_id"), 32) || !hex(response.at("stream_id"), 32) ||
        !nonnegative(response.at("confirmed_sequence")) || !nonnegative(response.at("retired_sequence")) ||
        response.at("confirmed_sequence") > kMaximumSequence ||
        response.at("producer_durability_verified") != false || response.at("shared_publication_mutated") != false)
        invalid("Confirmation schema is invalid.");

    const auto receipt_sequence = receipt.at("sequence").get<std::int64_t>();
    if (response.at("server_id") != receipt.at("server_id") || response.at("stream_id") != receipt.at("stream_id") ||
        response.at("confirmed_sequence") < receipt_sequence || response.at("retired_sequence") >= receipt_sequence)
        invalid("Confirmation does not match its receipt.");
}
} // namespace shin::observer_protocol
