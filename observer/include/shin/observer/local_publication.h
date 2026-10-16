#pragma once
#include "observer.h"

namespace shin::observer {
// External-only storage management. Never writes under a project root.
Json local_storage(const fs::path& home, const std::optional<fs::path>& root = {},
                   const std::string& revision = {}, const std::optional<bool>& automatic = {});
void guard_project_storage(const fs::path& home, const fs::path& root);
void guard_external_storage(const fs::path& home, const fs::path& root, const fs::path& output);
void automatic_local_storage(const fs::path& home);
void verify_storage_capacity(std::uintmax_t used, std::uintmax_t requested, std::uintmax_t available);
class LocalStorageReservation {
public:
    LocalStorageReservation(const fs::path& home, const fs::path& output, std::uintmax_t bytes,
                            const std::optional<fs::path>& new_root = {});
    ~LocalStorageReservation();
private:
    std::unique_ptr<StateStore> lock_;
};
// Private refresh candidate lifecycle belongs to publication ownership.
void recover_refresh_candidate(const fs::path& workspace);
class RefreshCandidate {
public:
    explicit RefreshCandidate(const fs::path& workspace);
    ~RefreshCandidate();
    RefreshCandidate(const RefreshCandidate&) = delete;
    RefreshCandidate& operator=(const RefreshCandidate&) = delete;
    fs::path path;
private:
    std::string identity;
    std::unique_ptr<StateStore> owner;
};

struct SnapshotRepairBase {
    std::string snapshot;
    std::string fingerprint;
    bool operator==(const SnapshotRepairBase&) const = default;
};

bool repairable_snapshot_error(const std::string& code);
// Caller holds an index read lock and has already verified publication authority.
// This returns a replacement precondition, never verified query evidence.
SnapshotRepairBase inspect_snapshot_repair(const Config& config, const std::string& expected_snapshot);

// Caller holds the project's StateStore writer for every mutating method.
class LocalPublication {
public:
    LocalPublication(const fs::path& workspace, const Json& binding, const Config& config, bool resume_setup = false);
    bool active() const;
    std::string phase() const;
    std::string token() const;
    std::string approved_profile_hash() const;
    std::string last_snapshot() const { return receipt().at("last_snapshot").get<std::string>(); }
    std::string approved_marker_hash() const;
    void accept_profile(const std::string& profile_hash);
    void accept_marker(const std::string& marker_hash);
    void enable(const std::string& snapshot, const std::string& profile_hash);
    std::optional<SnapshotRepairBase> reconcile();
    void verify() const;
    void preparing(const std::string& base, const std::string& candidate, const std::string& profile_hash = "");
    void committed(const std::string& snapshot);
private:
    Json& receipt();
    const Json& receipt() const;
    void save();
    fs::path directory_;
    Config config_;
    Json route_;
    std::optional<Json> record_;
};

Json revoke_local_publication(const fs::path& workspace, const Json& binding);
}
