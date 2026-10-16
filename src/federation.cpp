#include "context_internal.h"
#include "federation.h"
#include "generated_io.h"

namespace shin::context {

namespace {

constexpr std::uintmax_t kMaxFederationConfigBytes = 1024 * 1024;
constexpr std::size_t kMaxFederationMembers = 16;
constexpr std::size_t kMaxFederationQueryBytes = 16 * 1024;
constexpr std::size_t kMaxFederationTermGroups = 64;
constexpr std::size_t kMaxFederationExpandedTerms = 512;
constexpr int kMaxFederationLimit = 64;
constexpr int kMaxFederationPerDoc = 8;

struct FederationMember {
    std::string id;
    fs::path profile_dir;
    RuntimeOptions options;
};

struct FederationConfig {
    std::string id;
    fs::path path;
    std::string content_hash;
    std::vector<FederationMember> members;
};

struct SourceIdentity {
    std::string content_hash;
    long long bytes = 0;
};

struct FederatedMemberResult {
    FederationMember member;
    QueryRequest request;
    IndexFreshness freshness;
    BriefResult brief;
    BriefSelection selection;
    std::map<std::string, SourceIdentity> source_identities;
    std::string snapshot_id;
    int returned_records = 0;
};

struct FederatedEvidence {
    using Hit = std::variant<ContextHit, FactHit>;

    std::string primary_member;
    std::string profile;
    std::vector<std::string> member_ids;
    BriefEvidenceType type = BriefEvidenceType::section;
    int local_rank = 0;
    bool fallback_used = false;
    Hit hit;
    std::string dedup_key;
};

bool safe_config_id(std::string_view value) {
    return !value.empty() && value.size() <= 64 &&
           std::all_of(value.begin(), value.end(), [](char raw) {
               const unsigned char c = static_cast<unsigned char>(raw);
               return is_ascii_alnum(c) || raw == '-' || raw == '_';
           });
}

void reject_unknown_fields(const Json& object,
                           const std::set<std::string>& allowed,
                           const std::string& context) {
    if (!object.is_object()) {
        throw CliError("invalid-federation-config", "configuration",
                       context + " must be a JSON object.", 2, false,
                       "Repair the federation config and retry.");
    }
    for (auto field = object.begin(); field != object.end(); ++field) {
        if (allowed.find(field.key()) == allowed.end()) {
            throw CliError("invalid-federation-config", "configuration",
                           context + " contains unknown field: " + field.key(), 2, false,
                           "Remove unsupported federation fields and retry.");
        }
    }
}

fs::path resolve_existing_directory(const fs::path& base,
                                    const std::string& raw,
                                    const std::string& field) {
    if (raw.empty()) {
        throw CliError("invalid-federation-config", "configuration",
                       "Federation member is missing " + field + ".", 2, false,
                       "Provide every required member path.");
    }
    const fs::path candidate = fs::path(raw).is_absolute() ? fs::path(raw) : base / raw;
    std::error_code ec;
    const fs::path canonical = fs::weakly_canonical(candidate, ec);
    if (!ec && !fs::is_directory(canonical, ec)) {
        ec = std::make_error_code(std::errc::not_a_directory);
    }
    if (ec) {
        throw CliError("invalid-federation-path", "configuration",
                       "Federation " + field + " is not an existing directory: " +
                           candidate.generic_string(),
                       2, false, "Build the member index and repair the federation path.");
    }
    return canonical;
}

FederationConfig load_federation_config(const fs::path& path) {
    std::error_code ec;
    const fs::file_status status = fs::symlink_status(path, ec);
    if (ec || fs::is_symlink(status) || !fs::is_regular_file(status)) {
        throw CliError("invalid-federation-config", "configuration",
                       "Federation config must be a regular non-symlink file: " +
                           path.generic_string(),
                       2, false, "Provide a trusted local federation JSON file.");
    }
    const std::uintmax_t config_bytes = fs::file_size(path, ec);
    if (ec) {
        throw CliError("invalid-federation-config", "configuration",
                       "Cannot inspect federation config: " + path.generic_string(), 2, false,
                       "Verify the config file and retry.");
    }
    if (config_bytes > kMaxFederationConfigBytes) {
        throw CliError("federation-config-too-large", "resource",
                       "Federation config exceeds the 1 MiB limit.", 1, false,
                       "Reduce the federation member declaration.");
    }
    const std::string config_text = read_file(path);
    const Json root = parse_json_text(config_text, path.string());
    reject_unknown_fields(root, {"schemaVersion", "id", "members"}, "Federation config");
    if (json_string_value(root, "schemaVersion") != "shin.federation.v1") {
        throw CliError("invalid-federation-schema", "configuration",
                       "Federation config schemaVersion must be shin.federation.v1.", 2, false,
                       "Migrate the config to the supported federation schema.");
    }

    FederationConfig config;
    config.id = json_string_value(root, "id");
    config.path = fs::weakly_canonical(path);
    if (!safe_config_id(config.id)) {
        throw CliError("invalid-federation-id", "configuration",
                       "Federation id must use 1-64 ASCII letters, digits, '-' or '_'.", 2, false,
                       "Repair the federation id.");
    }
    const Json* members = find_json_field(root, "members");
    if (members == nullptr || !members->is_array() || members->empty() ||
        members->size() > kMaxFederationMembers) {
        throw CliError("invalid-federation-members", "configuration",
                       "Federation members must contain 1-16 entries.", 2, false,
                       "Provide a bounded non-empty member list.");
    }

    const fs::path base = config.path.parent_path();
    std::set<std::string> member_ids;
    std::vector<fs::path> output_roots;
    for (const Json& item : *members) {
        reject_unknown_fields(item, {"id", "profile", "profileDir", "root", "outDir"},
                              "Federation member");
        FederationMember member;
        member.id = json_string_value(item, "id");
        const std::string profile_id = json_string_value(item, "profile");
        if (!safe_config_id(member.id) || !safe_config_id(profile_id)) {
            throw CliError("invalid-federation-member", "configuration",
                           "Federation member id and profile must use bounded safe identifiers.", 2,
                           false, "Repair the member id and profile.");
        }
        if (!member_ids.insert(member.id).second) {
            throw CliError("duplicate-federation-member", "configuration",
                           "Duplicate federation member id: " + member.id, 2, false,
                           "Use one stable id per member.");
        }
        member.profile_dir = resolve_existing_directory(
            base, json_string_value(item, "profileDir"), "profileDir");
        member.options.root = resolve_existing_directory(
            base, json_string_value(item, "root"), "root");
        member.options.out_root = resolve_existing_directory(
            base, json_string_value(item, "outDir"), "outDir");
        bool duplicate_output = false;
        for (const fs::path& existing : output_roots) {
            std::error_code equivalent_ec;
            duplicate_output = fs::equivalent(existing, member.options.out_root, equivalent_ec);
            if (equivalent_ec) {
                throw CliError("invalid-federation-path", "configuration",
                               "Cannot compare federation outDir identities: " +
                                   equivalent_ec.message(),
                               2, false, "Verify member output paths and retry.");
            }
            if (duplicate_output) {
                break;
            }
        }
        if (duplicate_output) {
            throw CliError("duplicate-federation-index", "configuration",
                           "Federation members resolve to the same outDir: " +
                               member.options.out_root.generic_string(),
                           2, false, "Give every member one independently published index.");
        }
        output_roots.push_back(member.options.out_root);
        member.options.profile = make_profile(profile_id, member.profile_dir);
        member.options.force_index = true;
        config.members.push_back(std::move(member));
    }
    std::sort(config.members.begin(), config.members.end(),
              [](const FederationMember& left, const FederationMember& right) {
                  return left.id < right.id;
              });
    Json canonical_config = {
        {"schema_version", "shin.federation.v1"},
        {"id", config.id},
        {"members", Json::array()}
    };
    for (const FederationMember& member : config.members) {
        canonical_config["members"].push_back({
            {"id", member.id},
            {"profile", member.options.profile.id},
            {"profile_dir", member.profile_dir.generic_string()},
            {"root", member.options.root.generic_string()},
            {"out_root", member.options.out_root.generic_string()}
        });
    }
    config.content_hash = fnv1a64(canonical_config.dump());
    return config;
}

std::map<std::string, SourceIdentity> load_source_identities(const RuntimeOptions& options) {
    const fs::path manifest_path = options.out_root / kGeneratedDir / "manifest.json";
    const std::string manifest_text = read_file(manifest_path);
    const Json manifest = parse_json_text(manifest_text, manifest_path.string());
    std::map<std::string, SourceIdentity> identities;
    auto collect = [&](const char* field) {
        const Json* rows = find_json_field(manifest, field);
        if (rows == nullptr || !rows->is_array()) {
            return;
        }
        for (const Json& row : *rows) {
            const std::string path = json_string_value(row, "path");
            const std::string hash = json_string_value(row, "content_hash");
            if (path.empty() || hash.empty()) {
                continue;
            }
            long long bytes = 0;
            const Json* bytes_value = find_json_field(row, "bytes");
            if (bytes_value != nullptr && bytes_value->is_number_integer()) {
                bytes = bytes_value->get<long long>();
            }
            identities[path] = {hash, bytes};
        }
    };
    collect("sources");
    collect("source_fact_sources");
    return identities;
}

std::string retrieval_mode(const BriefResult& result) {
    if (result.directory_owner_scope_used) {
        return "directory-scoped";
    }
    if (result.document_owner_scope_used) {
        return "owner-scoped";
    }
    if (result.query_planner_used) {
        return "planned";
    }
    return result.fallback_used ? "relaxed" : "strict";
}

std::string matching_policy(const BriefResult& result) {
    if (result.directory_owner_scope_used) {
        return "directory-owner-scope-v1";
    }
    if (result.document_owner_scope_used) {
        return "document-owner-scope-v1";
    }
    if (result.query_planner_used) {
        return "document-coverage-planner-v1";
    }
    if (result.scope_fallback_used) {
        return "first-term-must-rest-should-v1";
    }
    return "required-groups-v1";
}

std::string evidence_type_name(BriefEvidenceType type) {
    switch (type) {
        case BriefEvidenceType::contract: return "contract";
        case BriefEvidenceType::decision: return "decision";
        case BriefEvidenceType::runbook: return "runbook";
        case BriefEvidenceType::fact: return "fact";
        case BriefEvidenceType::section: return "section";
    }
    return "section";
}

int evidence_type_order(BriefEvidenceType type) {
    return static_cast<int>(type);
}

const ContextHit* selected_section(const BriefSelection& selection, const BriefEvidenceRef& ref) {
    return ref.type == BriefEvidenceType::section ? &selection.sections.at(ref.index) : nullptr;
}

const FactHit* selected_fact(const BriefSelection& selection, const BriefEvidenceRef& ref) {
    switch (ref.type) {
        case BriefEvidenceType::contract: return &selection.contracts.at(ref.index);
        case BriefEvidenceType::decision: return &selection.decisions.at(ref.index);
        case BriefEvidenceType::runbook: return &selection.runbooks.at(ref.index);
        case BriefEvidenceType::fact: return &selection.facts.at(ref.index);
        case BriefEvidenceType::section: return nullptr;
    }
    return nullptr;
}

std::string_view evidence_path(const FederatedEvidence& evidence) {
    return std::visit([](const auto& hit) -> std::string_view {
        using Hit = std::decay_t<decltype(hit)>;
        if constexpr (std::is_same_v<Hit, FactHit>) {
            return hit.fact.path;
        } else {
            return hit.section.path;
        }
    }, evidence.hit);
}

int evidence_line(const FederatedEvidence& evidence) {
    return std::visit([](const auto& hit) {
        using Hit = std::decay_t<decltype(hit)>;
        if constexpr (std::is_same_v<Hit, FactHit>) {
            return hit.fact.line;
        } else {
            return hit.section.line;
        }
    }, evidence.hit);
}

std::string_view evidence_stable_id(const FederatedEvidence& evidence) {
    return std::visit([](const auto& hit) -> std::string_view {
        using Hit = std::decay_t<decltype(hit)>;
        if constexpr (std::is_same_v<Hit, FactHit>) {
            return hit.fact.id;
        } else {
            return hit.section.anchor;
        }
    }, evidence.hit);
}

int evidence_score(const FederatedEvidence& evidence) {
    return std::visit([](const auto& hit) { return hit.score; }, evidence.hit);
}

std::size_t evidence_matched_group_count(const FederatedEvidence& evidence) {
    return std::visit([](const auto& hit) { return hit.matched_group_indices.size(); },
                      evidence.hit);
}

void append_key_part(std::string& key, std::string_view value) {
    key += std::to_string(value.size());
    key.push_back(':');
    key.append(value);
}

std::string evidence_dedup_key(const FederatedEvidence& evidence,
                               const std::map<std::string, SourceIdentity>& identities) {
    const std::string path(evidence_path(evidence));
    const auto source = identities.find(path);
    std::string key;
    if (source == identities.end()) {
        append_key_part(key, evidence.primary_member);
    } else {
        append_key_part(key, source->second.content_hash);
        append_key_part(key, std::to_string(source->second.bytes));
    }
    append_key_part(key, path);
    append_key_part(key, evidence_type_name(evidence.type));
    if (const FactHit* hit = std::get_if<FactHit>(&evidence.hit); hit != nullptr) {
        const FactRow& fact = hit->fact;
        append_key_part(key, fact.evidence_kind);
        append_key_part(key, fact.verification_level);
        append_key_part(key, brief_fact_group(fact));
        append_key_part(key, fact.record_type);
        append_key_part(key, fact.id);
        append_key_part(key, std::to_string(fact.line));
        append_key_part(key, fact.title);
        append_key_part(key, fact.summary);
    } else {
        const SectionRow& section = std::get<ContextHit>(evidence.hit).section;
        append_key_part(key, section.evidence_kind);
        append_key_part(key, section.verification_level);
        append_key_part(key, section.row_type);
        append_key_part(key, std::to_string(section.start_line));
        append_key_part(key, std::to_string(section.end_line));
        append_key_part(key, section.heading);
        append_key_part(key, section.preview);
    }
    return key;
}

bool federated_evidence_better(const FederatedEvidence& left,
                               const FederatedEvidence& right) {
    if (left.local_rank != right.local_rank) {
        return left.local_rank < right.local_rank;
    }
    if (evidence_matched_group_count(left) != evidence_matched_group_count(right)) {
        return evidence_matched_group_count(left) > evidence_matched_group_count(right);
    }
    if (left.fallback_used != right.fallback_used) {
        return !left.fallback_used;
    }
    if (evidence_type_order(left.type) != evidence_type_order(right.type)) {
        return evidence_type_order(left.type) < evidence_type_order(right.type);
    }
    if (left.primary_member != right.primary_member) {
        return left.primary_member < right.primary_member;
    }
    if (evidence_path(left) != evidence_path(right)) {
        return evidence_path(left) < evidence_path(right);
    }
    if (evidence_line(left) != evidence_line(right)) {
        return evidence_line(left) < evidence_line(right);
    }
    return evidence_stable_id(left) < evidence_stable_id(right);
}

[[noreturn]] void throw_federation_invariant(const std::string& message);

FederatedEvidence make_federated_evidence(const FederatedMemberResult& member,
                                           const BriefEvidenceRef& ref,
                                           int local_rank) {
    FederatedEvidence::Hit hit = [&]() -> FederatedEvidence::Hit {
        if (const ContextHit* section = selected_section(member.selection, ref); section != nullptr) {
            return *section;
        }
        if (const FactHit* fact = selected_fact(member.selection, ref); fact != nullptr) {
            return *fact;
        }
        throw_federation_invariant("Selected federation evidence has no source hit.");
    }();
    FederatedEvidence evidence{
        member.member.id,
        member.member.options.profile.id,
        {member.member.id},
        ref.type,
        local_rank,
        member.brief.fallback_used,
        std::move(hit),
        ""
    };
    evidence.dedup_key = evidence_dedup_key(evidence, member.source_identities);
    return evidence;
}

std::vector<FederatedEvidence> merge_member_evidence(
    const std::vector<FederatedMemberResult>& members,
    int& candidate_records,
    int& deduplicated_records) {
    std::vector<FederatedEvidence> merged;
    std::map<std::string, std::size_t> key_to_index;
    for (const FederatedMemberResult& member : members) {
        for (std::size_t rank = 0; rank < member.selection.ordered.size(); ++rank) {
            ++candidate_records;
            FederatedEvidence evidence =
                make_federated_evidence(member, member.selection.ordered[rank],
                                        static_cast<int>(rank + 1));
            auto existing = key_to_index.find(evidence.dedup_key);
            if (existing == key_to_index.end()) {
                key_to_index[evidence.dedup_key] = merged.size();
                merged.push_back(std::move(evidence));
                continue;
            }
            ++deduplicated_records;
            FederatedEvidence& current = merged[existing->second];
            std::vector<std::string> member_ids = current.member_ids;
            append_unique_string(member_ids, evidence.primary_member);
            std::sort(member_ids.begin(), member_ids.end());
            if (federated_evidence_better(evidence, current)) {
                const std::string key = current.dedup_key;
                current = std::move(evidence);
                current.dedup_key = key;
            }
            current.member_ids = std::move(member_ids);
        }
    }
    std::sort(merged.begin(), merged.end(), federated_evidence_better);
    return merged;
}

std::vector<std::size_t> select_federated_evidence(const std::vector<FederatedEvidence>& evidence,
                                                   int limit) {
    std::vector<std::size_t> selected;
    std::set<std::size_t> seen;
    for (BriefEvidenceType type : {BriefEvidenceType::contract, BriefEvidenceType::decision,
                                   BriefEvidenceType::runbook, BriefEvidenceType::fact,
                                   BriefEvidenceType::section}) {
        auto candidate = std::find_if(evidence.begin(), evidence.end(),
                                      [type](const FederatedEvidence& item) {
                                          return item.type == type;
                                      });
        if (candidate == evidence.end()) {
            continue;
        }
        const std::size_t index = static_cast<std::size_t>(candidate - evidence.begin());
        selected.push_back(index);
        seen.insert(index);
        if (static_cast<int>(selected.size()) >= limit) {
            return selected;
        }
    }
    for (std::size_t index = 0; index < evidence.size() &&
                                static_cast<int>(selected.size()) < limit; ++index) {
        if (seen.insert(index).second) {
            selected.push_back(index);
        }
    }
    return selected;
}

Json index_freshness_document(const IndexFreshness& freshness) {
    return {
        {"status", freshness.status},
        {"mode", freshness.mode},
        {"snapshot_id", freshness.snapshot_id.empty() ? Json(nullptr) : Json(freshness.snapshot_id)},
        {"current", freshness.current},
        {"fully_verified", freshness.fully_verified},
        {"cache_hit", freshness.cache_hit},
        {"source_count", freshness.source_count},
        {"missing_files", freshness.missing_files},
        {"stale_files", freshness.stale_files},
        {"missing_sources", freshness.missing_sources},
        {"stale_sources", freshness.stale_sources},
        {"extra_sources", freshness.extra_sources},
        {"notes", freshness.notes}
    };
}

Json member_diagnostics_document(const BriefResult& result) {
    return {
        {"fallback_used", result.fallback_used},
        {"scope_fallback_used", result.scope_fallback_used},
        {"document_owner_scope_used", result.document_owner_scope_used},
        {"directory_owner_scope_used", result.directory_owner_scope_used},
        {"query_planner_used", result.query_planner_used},
        {"matching_policy", matching_policy(result)},
        {"candidate_rows", result.candidate_rows},
        {"candidate_index_used", result.candidate_index_used},
        {"required_group_matches", result.required_group_matches},
        {"total_term_groups", result.total_term_groups},
        {"planner_candidate_groups", result.planner_candidate_groups},
        {"planner_indexed_groups", result.planner_indexed_groups},
        {"planner_covered_groups", result.planner_covered_groups},
        {"planner_selected_documents", result.planner_selected_documents}
    };
}

Json section_hit_document(const ContextHit& hit) {
    const SectionRow& section = hit.section;
    return {
        {"path", section.path},
        {"line", section.line},
        {"status", section.status},
        {"authority_level", section.authority_level},
        {"evidence_kind", section.evidence_kind},
        {"verification_level", section.verification_level},
        {"start_line", section.start_line},
        {"end_line", section.end_line},
        {"level", section.level},
        {"row_type", section.row_type},
        {"chunk_index", section.chunk_index},
        {"heading", section.heading},
        {"score", hit.score},
        {"score_components", hit.score_components},
        {"matched_fields", hit.matched_fields},
        {"topics", section.topics},
        {"preview", section.preview}
    };
}

Json fact_hit_document(const FactHit& hit) {
    const FactRow& fact = hit.fact;
    return {
        {"id", fact.id},
        {"record_type", fact.record_type},
        {"path", fact.path},
        {"line", fact.line},
        {"start_line", fact.start_line},
        {"end_line", fact.end_line},
        {"score", hit.score},
        {"score_components", hit.score_components},
        {"matched_fields", hit.matched_fields},
        {"status", fact.status},
        {"area", fact.area},
        {"authority_level", fact.authority_level},
        {"evidence_kind", fact.evidence_kind},
        {"verification_level", fact.verification_level},
        {"title", fact.title},
        {"summary", fact.summary},
        {"source", fact.source},
        {"topics", fact.topics}
    };
}

void validate_query_options(const FederationQueryOptions& options) {
    if (trim(options.query).empty()) {
        throw_usage_error("missing-query", "A non-empty federated query is required.");
    }
    if (options.query.size() > kMaxFederationQueryBytes) {
        throw_usage_error("invalid-option-value", "Federated query must not exceed 16 KiB.");
    }
    if (options.limit < 1 || options.limit > kMaxFederationLimit) {
        throw_usage_error("invalid-option-value", "Federated limit must be in the range 1-64.");
    }
    if (options.per_doc_limit < 1 || options.per_doc_limit > kMaxFederationPerDoc) {
        throw_usage_error("invalid-option-value", "Federated per-document limit must be in the range 1-8.");
    }
}

[[noreturn]] void throw_federation_invariant(const std::string& message) {
    throw CliError("federation-invariant-violation", "internal", message, 1, false,
                   "Report the deterministic federation result and do not trust the partial output.");
}

void validate_result_invariants(const Json& result,
                                const std::vector<std::string>& configured_members) {
    const Json& evidence = result.at("evidence");
    const Json& members = result.at("members");
    const Json& budget = result.at("evidence_budget");
    const std::set<std::string> known_members(configured_members.begin(), configured_members.end());
    if (!evidence.is_array() || !members.is_array() ||
        members.size() != configured_members.size()) {
        throw_federation_invariant("Federation result member or evidence arrays are inconsistent.");
    }
    std::set<std::string> returned_members;
    for (const Json& member : members) {
        const std::string id = json_string_value(member, "id");
        if (known_members.find(id) == known_members.end() || !returned_members.insert(id).second) {
            throw_federation_invariant("Federation result contains an unknown or duplicate member.");
        }
    }
    if (returned_members != known_members) {
        throw_federation_invariant("Federation result member set differs from configuration.");
    }
    for (std::size_t index = 0; index < evidence.size(); ++index) {
        const Json& item = evidence[index];
        if (item.value("global_rank", 0U) != index + 1) {
            throw_federation_invariant("Federation global ranks are not contiguous.");
        }
        const std::string primary = json_string_value(item, "primary_member");
        const Json* provenance = find_json_field(item, "member_ids");
        if (known_members.find(primary) == known_members.end() || provenance == nullptr ||
            !provenance->is_array()) {
            throw_federation_invariant("Federation evidence has invalid primary provenance.");
        }
        bool primary_present = false;
        std::string previous_member;
        for (const Json& member : *provenance) {
            if (!member.is_string()) {
                throw_federation_invariant("Federation evidence references an unknown member.");
            }
            const std::string member_id = member.get<std::string>();
            if (known_members.find(member_id) == known_members.end() ||
                (!previous_member.empty() && member_id <= previous_member)) {
                throw_federation_invariant(
                    "Federation evidence provenance is unknown, duplicated, or unsorted.");
            }
            primary_present = primary_present || member_id == primary;
            previous_member = member_id;
        }
        if (!primary_present) {
            throw_federation_invariant("Federation primary member is absent from provenance.");
        }
    }
    const int candidates = budget.value("candidate_records", -1);
    const int deduplicated = budget.value("deduplicated_records", -1);
    const int eligible = budget.value("eligible_records", -1);
    const int returned = budget.value("returned_records", -1);
    const int budgeted_out = budget.value("budgeted_out_records", -1);
    if (candidates < 0 || deduplicated < 0 || eligible < 0 || returned < 0 ||
        budgeted_out < 0 || candidates - deduplicated != eligible ||
        eligible - returned != budgeted_out ||
        returned != static_cast<int>(evidence.size())) {
        throw_federation_invariant("Federation evidence budget arithmetic is inconsistent.");
    }
}

}  // namespace

struct FederationSession::Impl {
    explicit Impl(const fs::path& path) : config(load_federation_config(path)) {
        std::vector<fs::path> lock_roots;
        lock_roots.reserve(config.members.size());
        member_ids.reserve(config.members.size());
        for (const FederationMember& member : config.members) {
            lock_roots.push_back(member.options.out_root);
            member_ids.push_back(member.id);
        }
        std::sort(lock_roots.begin(), lock_roots.end());
        locks.reserve(lock_roots.size());
        for (const fs::path& out_root : lock_roots) {
            locks.push_back(lock_index_for_read(out_root));
        }
        for (const FederationMember& member : config.members) {
            if (shin::generated_io::publication_in_progress(member.options.out_root)) {
                throw_index_error("publication-incomplete",
                                  "Federation member snapshot publication is incomplete and requires writer recovery.");
            }
        }
    }

    FederationConfig config;
    std::vector<std::unique_ptr<shin::generated_io::IndexLock>> locks;
    std::vector<std::string> member_ids;
};

FederationSession::FederationSession(fs::path config_path)
    : impl_(std::make_unique<Impl>(std::move(config_path))) {}

FederationSession::~FederationSession() = default;
FederationSession::FederationSession(FederationSession&&) noexcept = default;
FederationSession& FederationSession::operator=(FederationSession&&) noexcept = default;

const std::string& FederationSession::id() const noexcept {
    return impl_->config.id;
}

const fs::path& FederationSession::config_path() const noexcept {
    return impl_->config.path;
}

const std::string& FederationSession::config_hash() const noexcept {
    return impl_->config.content_hash;
}

const std::vector<std::string>& FederationSession::member_ids() const noexcept {
    return impl_->member_ids;
}

Json FederationSession::brief(const FederationQueryOptions& requested_options) const {
    validate_query_options(requested_options);
    FederationQueryOptions options = requested_options;
    options.query = trim(std::move(options.query));

    std::vector<FederatedMemberResult> members;
    members.reserve(impl_->config.members.size());
    std::string snapshot_material = impl_->config.id;
    bool all_current = true;
    bool all_fully_verified = true;
    int source_count = 0;
    for (const FederationMember& member : impl_->config.members) {
        std::vector<std::string> query_args = {
            options.query, "--limit", std::to_string(options.limit),
            "--per-doc", std::to_string(options.per_doc_limit)};
        QueryRequest member_request = make_command_query_request(
            member.options.profile, query_args, options.limit, options.per_doc_limit);
        if (member_request.term_groups.size() > kMaxFederationTermGroups) {
            throw CliError("federation-query-too-broad", "resource",
                           "Federated query expands beyond 64 term groups.", 1, false,
                           "Use a more focused task query.");
        }
        std::size_t expanded_terms = 0;
        for (const std::vector<std::string>& group : member_request.term_groups) {
            expanded_terms += group.size();
        }
        if (expanded_terms > kMaxFederationExpandedTerms) {
            throw CliError("federation-query-too-broad", "resource",
                           "Federated query expands beyond 512 profile terms.", 1, false,
                           "Reduce aliases or use a more focused task query.");
        }

        const bool exact_freshness = options.verify_current || options.require_current;
        IndexFreshness freshness = assess_index_freshness(member.options, exact_freshness);
        if (options.require_current) {
            require_current_index_or_throw(freshness);
        }
        BriefResult brief = collect_brief_from_index_with_fallback(member.options, member_request);
        BriefSelection selection =
            select_brief_evidence(brief, options.limit, options.per_doc_limit);
        if (freshness.snapshot_id.empty()) {
            throw CliError("federation-snapshot-missing", "internal",
                           "Federation member has no published snapshot identity: " + member.id,
                           1, false, "Rebuild the member index before running federation.");
        }
        std::string snapshot_id = freshness.snapshot_id;
        std::map<std::string, SourceIdentity> identities = load_source_identities(member.options);
        snapshot_material += "\n" + member.id + "\n" + member.options.profile.id + "\n" + snapshot_id;
        all_current = all_current && freshness.current;
        all_fully_verified = all_fully_verified && freshness.fully_verified;
        source_count += freshness.source_count;
        members.push_back({member, std::move(member_request), std::move(freshness),
                           std::move(brief), std::move(selection), std::move(identities),
                           std::move(snapshot_id), 0});
    }

    int candidate_records = 0;
    int deduplicated_records = 0;
    std::vector<FederatedEvidence> evidence =
        merge_member_evidence(members, candidate_records, deduplicated_records);
    const std::vector<std::size_t> selected =
        select_federated_evidence(evidence, options.limit);
    for (std::size_t index : selected) {
        for (const std::string& member_id : evidence[index].member_ids) {
            auto member = std::find_if(members.begin(), members.end(),
                                       [&](const FederatedMemberResult& item) {
                                           return item.member.id == member_id;
                                       });
            if (member != members.end()) {
                ++member->returned_records;
            }
        }
    }

    Json result = make_result_document("shin.context-federated-brief.v1", "federated-brief", true);
    result["federation"] = {
        {"id", impl_->config.id},
        {"config", impl_->config.path.generic_string()},
        {"config_hash", impl_->config.content_hash},
        {"member_count", members.size()},
        {"snapshot_set_id", fnv1a64(snapshot_material)},
        {"snapshot_consistency", "cooperating-local-index-set-v1"}
    };
    result["query"] = options.query;
    result["require_current"] = options.require_current;
    result["index_freshness"] = {
        {"current", all_current},
        {"fully_verified", all_fully_verified},
        {"member_count", members.size()}
    };
    result["evidence_budget"] = {
        {"policy", "global-record-limit-with-type-seeding-and-local-rank-fusion-v1"},
        {"max_records", options.limit},
        {"candidate_records", candidate_records},
        {"deduplicated_records", deduplicated_records},
        {"eligible_records", evidence.size()},
        {"returned_records", selected.size()},
        {"budgeted_out_records", static_cast<int>(evidence.size()) -
                                      static_cast<int>(selected.size())}
    };
    result["source_count"] = source_count;
    result["members"] = Json::array();
    for (const FederatedMemberResult& member : members) {
        result["members"].push_back({
            {"id", member.member.id},
            {"profile", member.member.options.profile.id},
            {"profile_dir", member.member.profile_dir.generic_string()},
            {"root", member.member.options.root.generic_string()},
            {"out_root", member.member.options.out_root.generic_string()},
            {"snapshot_id", member.snapshot_id},
            {"index_freshness", index_freshness_document(member.freshness)},
            {"retrieval_mode", retrieval_mode(member.brief)},
            {"query_diagnostics", member_diagnostics_document(member.brief)},
            {"candidate_records", member.selection.ordered.size()},
            {"returned_records", member.returned_records}
        });
    }
    result["evidence"] = Json::array();
    for (std::size_t rank = 0; rank < selected.size(); ++rank) {
        const FederatedEvidence& item = evidence[selected[rank]];
        result["evidence"].push_back({
            {"global_rank", rank + 1},
            {"primary_member", item.primary_member},
            {"member_ids", item.member_ids},
            {"profile", item.profile},
            {"evidence_type", evidence_type_name(item.type)},
            {"local_rank", item.local_rank},
            {"score_scope", "member-local"},
            {"score", evidence_score(item)},
            {"hit", std::holds_alternative<FactHit>(item.hit)
                        ? fact_hit_document(std::get<FactHit>(item.hit))
                        : section_hit_document(std::get<ContextHit>(item.hit))}
        });
    }
    validate_result_invariants(result, impl_->member_ids);
    return result;
}

}  // namespace shin::context
