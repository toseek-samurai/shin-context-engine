#pragma once

#include "shin/server/model.h"
#include "shin/server/checkpoint.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace pqxx {
class transaction_base;
}

namespace shin::server {

struct RetrievalRoute;
struct VerifiedOidcIdentity;

enum class ObserverReclamationMode { confirmed, discard_unconfirmed };
enum class IdentityMaintenance { recovery, observer_retirement };

class Store {
public:
    explicit Store(StoreConfig config);
    ~Store();

    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;
    Store(Store&&) = delete;
    Store& operator=(Store&&) = delete;

    static void migrate(const StoreConfig& config);
    static Json recover(const StoreConfig& config, const Json& input, bool apply,
                        IdentityMaintenance mode = IdentityMaintenance::recovery);

    Json readiness();
    Json identity_register_account(const Principal& actor, const Json& input);
    void identity_request_admission(const Principal& actor, bool directory_request);
    Json identity_account(const Principal& actor);
    Json identity_accounts(const Principal& actor, const std::string& after, std::size_t limit);
    Json identity_bind_external(const Principal& actor, const std::string& account, const Json& input);
    Json identity_external(const Principal& actor, const std::string& account);
    Json identity_external_state(const Principal& actor, const std::string& account, const Json& input);
    // Authentication only: caller must already have verified issuer/token/JWKS.
    // Device and workspace admission are separate; this is not an HTTP login.
    Principal identity_resolve_external(const VerifiedOidcIdentity& identity);
    // Verified token sid, not a caller-supplied device ID. Used by explicit HTTP login.
    Json identity_register_device(const VerifiedOidcIdentity& identity, const Json& input);
    Principal identity_resolve_device(const VerifiedOidcIdentity& identity);
    Json identity_devices(const Principal& actor, const std::string& account,
                          const std::string& after, std::size_t limit);
    Json identity_revoke_device(const Principal& actor, const std::string& account,
                               const std::string& device, const Json& input);
    Json identity_revoke_devices(const Principal& actor, const std::string& account);
    Json identity_account_state(const Principal& actor, const std::string& id, const Json& input);
    Json identity_register_tenant(const Principal& actor, const Json& input);
    Json identity_tenants(const Principal& actor, const std::string& after, std::size_t limit);
    Json identity_tenant(const Principal& actor, const std::string& tenant);
    Json identity_membership(const Principal& actor, const std::string& tenant, const Json& input);
    Json identity_bind_project(const Principal& actor, const std::string& tenant, const Json& input);
    Json identity_project(const Principal& actor, const std::string& tenant, const std::string& project);
    Json identity_projects(const Principal& actor, const std::string& tenant, const std::string& after, std::size_t limit);
    Json identity_bind_workspace(const Principal& actor, const std::string& tenant, const std::string& project, const Json& input);
    Json identity_workspace(const Principal& actor, const std::string& tenant, const std::string& project, const std::string& workspace);
    Json identity_workspaces(const Principal& actor, const std::string& tenant, const std::string& project,
                             const std::string& after, std::size_t limit);
    Json recovery_status(const Principal& principal);
    RegistrationResult register_project(const Principal& principal,
                                        const ProjectInput& project,
                                        std::string_view idempotency_key,
                                        std::string_view request_hash);
    Json list_projects(const Principal& principal, std::string_view after, std::size_t limit);
    Json get_project(const Principal& principal, std::string_view id);
    // Private canonical payloads and replay metadata are internal service inputs, never HTTP responses.
    Json checkpoint_record(const Principal& principal, const std::string& project, const std::string& slot);
    Json scoped_checkpoint_record(const Principal& principal, const std::string& project, const std::string& slot,
                                  const WorkspaceReadRequest& scope);
    Json scoped_checkpoints(const Principal& principal, const std::string& project, const std::string& after,
                            std::size_t limit, const WorkspaceReadRequest& scope);
    Json checkpoint_replay(const Principal& principal, const std::string& project, const std::string& slot,
                           const CheckpointMutation& mutation, const std::string& operation,
                           const std::optional<WorkspaceReadRequest>& scope = std::nullopt);
    Json save_checkpoint(const Principal& principal, const std::string& project, const std::string& slot,
                         const CheckpointMutation& mutation, const Json& provenance, const std::string& checkpoint,
                         const std::optional<WorkspaceReadRequest>& scope = std::nullopt, const Json& admission = Json());
    Json clear_checkpoint(const Principal& principal, const std::string& project, const std::string& slot,
                          const CheckpointMutation& mutation, const std::optional<WorkspaceReadRequest>& scope = std::nullopt);
    Json import_checkpoint_receipts(const Principal& principal, const std::string& project, const std::string& slot,
                                    const CheckpointMutation& mutation, const Json& receipts,
                                    const std::optional<WorkspaceReadRequest>& scope = std::nullopt);
    Json list_checkpoints(const Principal& principal, const std::string& project, const std::string& after,
                          std::size_t limit);
    Json prune_checkpoints(const Principal& principal, const std::string& project,
                           const std::optional<WorkspaceReadRequest>& scope = std::nullopt);
    Json register_workspace(const Principal& principal, const std::string& project_id,
                            const WorkspaceInput& workspace, std::string_view idempotency_key,
                            std::string_view request_hash);
    Json workspace_registration_replay(const Principal& principal, const std::string& project_id,
                                       std::string_view idempotency_key, std::string_view request_hash);
    Json get_workspace(const Principal& principal, const std::string& project_id, const std::string& id);
    // Internal scoped read inputs; host-private rows must not become HTTP metadata.
    Json scoped_workspace(const Principal& principal, const std::string& project, const WorkspaceReadRequest& scope);
    Json scoped_active_route(const Principal& principal, const RetrievalRoute& route, const WorkspaceReadRequest& scope);
    Json scoped_publication_head(const Principal& principal, const std::string& project,
                                 const std::string& publication, const WorkspaceReadRequest& scope);
    Json scoped_publication_proposal(const Principal& principal, const std::string& project,
                                    const std::string& publication, std::uint64_t id, const WorkspaceReadRequest& scope);
    Json scoped_publication_proposals(const Principal& principal, const std::string& project,
                                     const std::string& publication, std::uint64_t after, std::size_t limit,
                                     const WorkspaceReadRequest& scope);
    Json list_workspaces(const Principal& principal, const std::string& project_id,
                         const std::string& after, std::size_t limit);
    Json audit_events(const Principal& principal, std::uint64_t after, std::size_t limit);
    Json capture_retrieval_journal(const Principal& principal, const Json& batch);
    Json register_journal_producer(const Principal& principal, const Json& provenance);
    Json plan_journal_reclamation(const Principal& principal, std::uint64_t through);
    Json apply_journal_reclamation(const Principal& principal, const Json& input);
    Json retrieval_journal(const Principal& principal, const std::string& project, std::uint64_t after, std::size_t limit);
    Json scoped_retrieval_journal(const Principal& principal, const std::string& project,
                                  const WorkspaceReadRequest& scope, std::uint64_t after, std::size_t limit);
    Json scoped_retrieval_journal_attempt(const Principal& principal, const std::string& project,
                                          const WorkspaceReadRequest& scope, std::uint64_t id);
    Json retrieval_journal_attempt(const Principal& principal, std::uint64_t id);
    Json retrieval_journal_report(const Principal& principal, const std::string& project, bool query_groups);
    Json retrieval_journal_producers(const Principal& principal, const std::string& after, std::size_t limit);
    Json plan_journal_retention(const Principal& principal, const std::string& instance, std::uint64_t through);
    Json apply_journal_retention(const Principal& principal, const std::string& instance, const Json& input);
    Json audit_history(const Principal& principal);
    Json export_audit(const Principal& principal, std::size_t limit);
    Json apply_audit_retention(const Principal& principal, const Json& input);
    Json observer_identity(const Principal& principal);
    Json confirm_observer(const Principal& principal, const std::string& stream, const Json& input, const std::optional<ObserverScope>& scope = std::nullopt);
    Json observer_retention(const Principal& principal, const std::string& stream, const std::optional<ObserverScope>& scope = std::nullopt);
    Json apply_observer_retention(const Principal& principal, const std::string& stream, const Json& input, const std::optional<ObserverScope>& scope = std::nullopt);
    Json observer_reclamation(const Principal& principal, const std::string& stream,
                             ObserverReclamationMode mode = ObserverReclamationMode::confirmed, const std::optional<ObserverScope>& scope = std::nullopt);
    Json apply_observer_reclamation(const Principal& principal, const std::string& stream, const Json& input,
                                   ObserverReclamationMode mode = ObserverReclamationMode::confirmed, const std::optional<ObserverScope>& scope = std::nullopt);
    Json reconcile_observer(const Principal& principal, const std::string& stream, const Json& input, const std::optional<ObserverScope>& scope = std::nullopt);
    Json enroll_observer(const Principal& principal, const Json& enrollment, const std::optional<ObserverScope>& scope = std::nullopt);
    Json get_observer(const Principal& principal, const std::string& stream_id, const std::optional<ObserverScope>& scope = std::nullopt);
    Json revoke_observer(const Principal& principal, const std::string& stream_id, const std::optional<ObserverScope>& scope = std::nullopt);
    Json ingest_observation(const Principal& principal, const std::string& stream_id, const Json& submission, const std::optional<ObserverScope>& scope = std::nullopt);
    Json enqueue_preparation(const Principal& principal,
                             const std::string& project_id,
                             const std::string& route_id,
                             const std::string& route_fingerprint,
                             const std::string& expected_snapshot_id,
                             std::string_view key,
                             std::string_view request_hash,
                             std::size_t max_retained_jobs,
                             std::uint64_t artifact_reservation_bytes,
                             std::string_view artifact_namespace,
                             const std::function<std::string()>& artifact_key_factory,
                             const std::optional<WorkspaceReadRequest>& scope = std::nullopt, const Json& admission = Json());
    Json preparation_admission(const Principal& principal, const std::string& project, const WorkspaceReadRequest& scope);
    WorkspaceWriteScope capture_workspace_write(const Principal& principal, const std::string& project, const WorkspaceReadRequest& scope);
    Json scoped_preparation(const Principal& principal, const std::string& project, std::uint64_t id,
                            const WorkspaceReadRequest& scope);
    Json scoped_preparations(const Principal& principal, const std::string& project, std::uint64_t after,
                             std::size_t limit, const WorkspaceReadRequest& scope);
    Json get_preparation(const Principal& principal, std::uint64_t id);
    // Internal inspection input: authorization precedes access; never return this row over HTTP.
    Json preparation_inspection_input(const Principal& principal, std::uint64_t id,
                                      const std::optional<WorkspaceWriteScope>& write = std::nullopt);
    // Cleanup-only private metadata; obsolete admission does not permit candidate reuse.
    Json preparation_cleanup_input(const Principal& principal, std::uint64_t id,
                                   const std::optional<WorkspaceWriteScope>& write = std::nullopt);
    Json list_preparations(const Principal& principal, std::uint64_t after, std::size_t limit);
    Json cancel_preparation(const Principal& principal, std::uint64_t id,
                            const std::optional<WorkspaceWriteScope>& write = std::nullopt);
    Json begin_retention(const Principal& principal,
                         std::uint64_t id,
                         const Json& expected_input,
                         const std::string& plan_id,
                         const Json& inventory,
                         const std::optional<WorkspaceWriteScope>& write = std::nullopt);
    Json finish_retention(const Principal& principal,
                          std::uint64_t id,
                          const std::string& plan_id,
                          const std::string& artifact_namespace,
                          const std::string& artifact_key,
                          const std::optional<WorkspaceWriteScope>& write = std::nullopt);

    Json claim_preparation(const std::string& worker_id, int lease_seconds);
    bool renew_preparation(std::uint64_t id, const std::string& worker_id, int lease_seconds);
    bool finish_preparation(std::uint64_t id,
                            const std::string& worker_id,
                            const std::string& terminal_state,
                            const std::string& reason_code,
                            const std::string& artifact_snapshot_id,
                            std::uint64_t artifact_bytes);
    void expire_preparations();

    // Internal artifact binding is never an HTTP response.
    Json active_route(const Principal& principal, const std::string& project, const std::string& route);
    Json activation_replay(const Principal& principal, const std::string& project, const std::string& route,
                           std::uint64_t expected_revision, const Json& preparation_id,
                           const std::optional<WorkspaceWriteScope>& write = std::nullopt);
    Json commit_activation(const Principal& principal, const std::string& project, const std::string& route,
                           std::uint64_t expected_revision, const Json& verified_input,
                           const std::optional<WorkspaceWriteScope>& write = std::nullopt);
    bool preparation_is_active(const Principal& principal, std::uint64_t id,
                               const std::optional<WorkspaceWriteScope>& write = std::nullopt);

    Json publication_submission_replay(const Principal& principal, const std::string& project,
                                      const std::string& publication, std::uint64_t preparation,
                                      std::uint64_t base_revision, const std::string& key, const std::string& hash,
                                      std::uint64_t history_epoch = 0, const std::optional<WorkspaceWriteScope>& write = std::nullopt);
    // source_binding is server-verified catalog metadata, never an HTTP request payload.
    Json submit_publication_proposal(const Principal& principal, const std::string& project,
                                    const std::string& publication, std::uint64_t preparation,
                                    std::uint64_t base_revision, const std::string& key, const std::string& hash,
                                    const Json& source_binding, std::uint64_t history_epoch = 0,
                                    const std::optional<WorkspaceWriteScope>& write = std::nullopt);
    // Internal selection includes artifact bindings; HTTP must project public metadata only.
    Json publication_head(const Principal& principal, const std::string& project, const std::string& publication,
                          bool include_admission = false);
    Json publication_history(const Principal& principal, const std::optional<WorkspaceWriteScope>& write = std::nullopt);
    Json plan_publication_retention(const Principal& principal, std::size_t limit,
                                    const std::optional<WorkspaceWriteScope>& write = std::nullopt);
    Json apply_publication_retention(const Principal& principal, const Json& input,
                                     const std::optional<WorkspaceWriteScope>& write = std::nullopt);
    Json withdraw_publication(const Principal& principal, const std::string& project,
                              const std::string& publication, std::uint64_t expected_revision,
                              const std::optional<WorkspaceWriteScope>& write = std::nullopt);
    Json publication_proposal(const Principal& principal, const std::string& project,
                              const std::string& publication, std::uint64_t id);
    Json publication_review_input(const Principal& principal, const std::string& project,
                                  const std::string& publication, std::uint64_t id);
    Json list_publication_proposals(const Principal& principal, const std::string& project,
                                   const std::string& publication, std::uint64_t after, std::size_t limit);
    Json publication_resolution_replay(const Principal& principal, const std::string& project,
                                      const std::string& publication, std::uint64_t id,
                                      const std::string& decision, std::uint64_t expected_revision,
                                      const std::optional<WorkspaceWriteScope>& write = std::nullopt);
    Json resolve_publication_proposal(const Principal& principal, const std::string& project,
                                     const std::string& publication, std::uint64_t id,
                                     const std::string& decision, std::uint64_t expected_revision,
                                     const Json& verified_input, const std::optional<WorkspaceWriteScope>& write = std::nullopt);

private:
    class Impl;

    Json run_transaction(std::function<Json(pqxx::transaction_base&)> operation);

    std::string source_host_id_;
    std::unique_ptr<Impl> impl_;
};

} // namespace shin::server
