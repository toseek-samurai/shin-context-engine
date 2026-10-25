#include "shin/observer/local_publication.h"
#include "generated_io.h"
#include "context_internal.h"
#include "snapshot_verification.h"
#include "source_cache.h"
#include "shin/observer/local_registry.h"
#include "shin/observer/local_service.h"
#include <sqlite3.h>
#include <set>
#include <array>
#include <chrono>
#include <thread>
#ifdef _WIN32
#include "shin/windows/file.h"
#include <windows.h>
#include <bcrypt.h>
#else
#include <openssl/rand.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace shin::observer {
namespace {
namespace io = shin::generated_io;
namespace core = shin::context;
bool candidate_present(const fs::path& path) {
    std::error_code error;
    const auto status = fs::symlink_status(path, error);
    if (error && error != std::errc::no_such_file_or_directory)
        throw Error("path-unavailable", "Cannot inspect a local project path.");
    return fs::exists(status);
}
bool entry_exists(const fs::path& path) { return fs::exists(fs::symlink_status(path)); }
void require(bool condition, const char* code, const char* message) { if (!condition) throw Error(code, message); }
bool hexadecimal(const Json& value, std::size_t size) {
    return value.is_string() && value.get_ref<const std::string&>().size() == size &&
        value.get_ref<const std::string&>().find_first_not_of("0123456789abcdef") == std::string::npos;
}
bool snapshot(const Json& value) {
    return value.is_string() && value.get_ref<const std::string&>().size() == 24 &&
        value.get_ref<const std::string&>().starts_with("fnv1a64:") && hexadecimal(value.get<std::string>().substr(8), 16);
}
std::optional<Json> read_record(const fs::path& directory, bool allow_empty = false) {
    if (!entry_exists(directory)) return std::nullopt;
    StateStore store(directory, false);
    auto record = store.load();
    if (!record && allow_empty) {
        for (const auto& entry : fs::directory_iterator(directory))
            require(entry.path().filename() == "observer.lock" || entry.path().filename() == "state.pending",
                "refresh-grant-incomplete", "Unbound publisher directory contains unexpected data.");
        return std::nullopt;
    }
    if (!record) throw Error("refresh-grant-incomplete", "Publisher setup was interrupted; retry explicit managed init.");
    const auto& value = *record;
    const bool transactional = value.is_object() && value.value("schema_version", Json(nullptr)) == "shin.local-publication.v4";
    const bool marker_revised = transactional || (value.is_object() && value.value("schema_version", Json(nullptr)) == "shin.local-publication.v3");
    const bool revised = marker_revised || (value.is_object() && value.value("schema_version", Json(nullptr)) == "shin.local-publication.v2");
    require(value.is_object() && value.size() == (transactional ? 9 : marker_revised ? 8 : revised ? 7 : 6) &&
        (revised || value.value("schema_version", Json(nullptr)) == "shin.local-publication.v1") &&
        (!revised || (value.contains("approved_profile_hash") && snapshot(value.at("approved_profile_hash")))) &&
        (!marker_revised || (value.contains("approved_marker_hash") && (hexadecimal(value.at("approved_marker_hash"), 64) || (transactional && value.at("approved_marker_hash") == "")))) &&
        (!transactional || (value.contains("pending_profile_hash") &&
            (value.at("pending_profile_hash") == "" || (snapshot(value.at("pending_profile_hash")) && value.value("pending_snapshot", Json("")) != "")))) &&
        value.contains("token") && hexadecimal(value.at("token"), 64) && value.contains("route") && value.at("route").is_object() &&
        value.contains("last_snapshot") && snapshot(value.at("last_snapshot")) && value.contains("pending_snapshot") &&
        (value.at("pending_snapshot") == "" || snapshot(value.at("pending_snapshot"))),
        "refresh-grant-invalid", "Publisher receipt is invalid; preserve it for review.");
    const auto phase = value.value("phase", "");
    require(phase == "granting" || phase == "active" || phase == "revoking" || phase == "revoked",
        "refresh-grant-invalid", "Publisher phase is invalid.");
    return record;
}
std::optional<std::string> owner(const fs::path& out) {
    try { return io::publisher_owner(out); }
    catch (const std::exception&) { throw Error("refresh-owner-invalid", "Index publisher ownership is unsafe or incomplete."); }
}
std::string new_token() {
    std::array<unsigned char, 32> bytes{};
#ifdef _WIN32
    const bool random = BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
#else
    const bool random = RAND_bytes(bytes.data(), bytes.size()) == 1;
#endif
    if (!random) throw Error("refresh-grant-failed", "Cannot create publisher identity.");
    std::string result;
    for (auto byte : bytes) { result.push_back("0123456789abcdef"[byte >> 4]); result.push_back("0123456789abcdef"[byte & 15]); }
    return result;
}

void owned_repair_entry(const fs::path& path, bool directory) {
#ifdef _WIN32
    try {
        if (directory) { windows::Directory checked(path, windows::Directory::Access::user_owned); }
        else { windows::ReadFile checked(path); checked.require_user_owned(); }
    } catch (const std::exception&) {
        throw Error("refresh-repair-unsafe", "Repair requires owned entries without links or shared write access.");
    }
#else
    struct stat status{};
    require(::lstat(path.c_str(), &status) == 0 && status.st_uid == ::geteuid() &&
        (status.st_mode & 0022) == 0 &&
        (directory ? S_ISDIR(status.st_mode) : S_ISREG(status.st_mode) && status.st_nlink == 1),
        "refresh-repair-unsafe", "Repair requires owned entries without links or shared write access.");
#endif
}

std::string repair_file(const fs::path& path, std::uintmax_t limit) {
    owned_repair_entry(path, false);
    const auto text = read_text(path, limit, false);
    owned_repair_entry(path, false);
    return text;
}
}

void recover_refresh_candidate(const fs::path& workspace) {
    const auto directory = workspace / "refresh";
    if (!candidate_present(directory)) return;
    StateStore store(directory, true);
    const auto saved = store.load();
    const auto candidate = directory / "candidate";
    if (!saved) {
        if (candidate_present(candidate)) throw Error("refresh-storage-unowned", "Unattributed refresh data requires review.");
        store.recover_pending();
        return;
    }
    if (!saved->is_object() || saved->size() != 3 ||
        saved->value("schema_version", "") != "shin.local-refresh-candidate.v1" ||
        !saved->contains("identity") || !saved->at("identity").is_string())
        throw Error("refresh-storage-invalid", "Refresh ownership receipt is invalid.");
    const auto phase = saved->value("phase", "");
    if (phase != "idle" && phase != "creating" && phase != "prepared")
        throw Error("refresh-storage-invalid", "Refresh ownership phase is invalid.");
    if (candidate_present(candidate)) {
        const auto identity = directory_identity(candidate, true);
        const bool empty_creation = phase == "creating" && saved->at("identity") == "" && fs::is_empty(candidate);
        if (!empty_creation && (phase != "prepared" || saved->at("identity") != identity))
            throw Error("refresh-storage-changed", "Refresh candidate was replaced; automatic removal is refused.");
        std::error_code error;
        fs::remove_all(candidate, error);
        if (error) throw Error("refresh-storage-unavailable", "Cannot reclaim owned refresh candidate.");
    }
    store.recover_pending();
    if (phase != "idle")
        store.save({{"schema_version", "shin.local-refresh-candidate.v1"}, {"phase", "idle"}, {"identity", ""}});
}

RefreshCandidate::RefreshCandidate(const fs::path& workspace) {
    recover_refresh_candidate(workspace);
    const auto directory = workspace / "refresh";
    ensure_private_directory(directory);
    owner = std::make_unique<StateStore>(directory, true);
    owner->recover_pending();
    owner->save({{"schema_version", "shin.local-refresh-candidate.v1"}, {"phase", "creating"}, {"identity", ""}});
    path = directory / "candidate";
    try { ensure_private_directory(path, true); }
    catch (const Error&) { throw Error("refresh-storage-unavailable", "Cannot create a private refresh candidate."); }
    identity = directory_identity(path, true);
    owner->save({{"schema_version", "shin.local-refresh-candidate.v1"}, {"phase", "prepared"}, {"identity", identity}});
}

RefreshCandidate::~RefreshCandidate() {
    try {
        if (directory_identity(path, true) == identity) {
            std::error_code error;
            fs::remove_all(path, error);
            if (!error) owner->save({{"schema_version", "shin.local-refresh-candidate.v1"}, {"phase", "idle"}, {"identity", ""}});
        }
    } catch (...) {
        // Replaced or unverifiable directories are not ours to remove.
        return;
    }
}

bool repairable_snapshot_error(const std::string& code) {
    return code == "snapshot-integrity-failed" || code == "snapshot-file-invalid" ||
           code == "snapshot-integrity-bijection-failed";
}

SnapshotRepairBase inspect_snapshot_repair(const Config& config, const std::string& expected_snapshot) {
    require(snapshot(expected_snapshot) && !io::publication_in_progress(config.out_dir),
        "refresh-repair-unattributed", "Repair requires a committed publication without an incomplete transaction.");
    const auto generated = config.out_dir / core::kGeneratedDir;
    (void)directory_identity(config.out_dir, true);
    owned_repair_entry(generated, true);
    const auto identity = directory_identity(generated);
    const auto manifest_path = generated / "manifest.json";
    const auto manifest = repair_file(manifest_path, std::min<std::uintmax_t>(config.max_file_bytes, 64U * 1024U * 1024U));
    require(core::fnv1a64(manifest) == expected_snapshot,
        "refresh-repair-unattributed", "The manifest is not the previously committed publication.");
    // The exact manifest was already verified when its snapshot was committed.
    // Use its output allowlist, but never treat damaged payloads as valid data.
    const auto document = core::parse_json_text(manifest, "repair manifest");
    const auto& outputs = document.at("outputs");
    require(outputs.is_array() && outputs.size() <= 1025, "refresh-repair-layout", "Repair output inventory is invalid.");
    std::set<std::string> files, directories;
    for (const auto& row : outputs) {
        const auto path = row.at("path").get<std::string>();
        const auto relative = fs::path(path).lexically_relative(core::kGeneratedDir);
        require(core::profile_relative_path_valid(path) && !relative.empty() &&
            relative.begin()->string() != ".." && relative != "." && files.insert(path).second,
            "refresh-repair-layout", "Repair output path is outside the committed inventory.");
        for (auto parent = fs::path(path).parent_path(); parent != core::kGeneratedDir; parent = parent.parent_path())
            directories.insert(parent.generic_string());
    }
    require(files.contains((core::kGeneratedDir / "manifest.json").generic_string()),
        "refresh-repair-layout", "Repair inventory has no manifest.");
    Json observed = Json::object();
    std::uintmax_t bytes = 0;
    std::size_t entries = 0;
    for (const auto& entry : fs::recursive_directory_iterator(generated)) {
        require(++entries <= 2048, "refresh-repair-budget", "Repair layout exceeds its bounded inventory.");
        const auto path = entry.path().lexically_relative(config.out_dir).generic_string();
        const auto status = entry.symlink_status();
        if (fs::is_directory(status)) {
            require(directories.contains(path), "refresh-repair-layout", "Unexpected directory prevents automatic repair.");
            owned_repair_entry(entry.path(), true);
            observed[path] = directory_identity(entry.path());
        } else {
            require(fs::is_regular_file(status) && files.contains(path),
                "refresh-repair-layout", "Unexpected or unsafe file prevents automatic repair.");
            const auto content = repair_file(entry.path(), std::min(config.max_file_bytes, config.max_snapshot_bytes - bytes));
            bytes += content.size();
            observed[path] = digest(content);
        }
    }
    owned_repair_entry(generated, true);
    require(directory_identity(generated) == identity &&
        repair_file(manifest_path, config.max_file_bytes) == manifest,
        "refresh-base-changed", "Publication changed during repair inspection.");
    return {expected_snapshot, digest(Json{{"directory", identity}, {"files", observed}}.dump())};
}

LocalPublication::LocalPublication(const fs::path& workspace, const Json& binding, const Config& config, bool resume_setup)
    : directory_(workspace / "publication"), config_(config), route_(binding), record_(read_record(directory_, resume_setup)) {
    route_.erase("prepared_snapshot");
    route_["output"] = config.out_dir.string();
    route_["home_identity"] = directory_identity(workspace.parent_path().parent_path(), true);
    if (record_ && record_->at("phase") != "revoked")
        require(record_->at("route") == route_, "refresh-grant-changed", "Granted route changed; release ownership before reviewing its new settings.");
}
bool LocalPublication::active() const { return record_ && record_->at("phase") == "active"; }
std::string LocalPublication::phase() const { return record_ ? record_->at("phase").get<std::string>() : "read-only"; }
Json& LocalPublication::receipt() {
    if (!record_) throw Error("refresh-grant-required", "Publisher permission is missing.");
    return *record_;
}
const Json& LocalPublication::receipt() const {
    if (!record_) throw Error("refresh-grant-required", "Publisher permission is missing.");
    return *record_;
}
std::string LocalPublication::token() const { return active() ? receipt().at("token").get<std::string>() : ""; }
std::string LocalPublication::approved_profile_hash() const {
    if (record_ && record_->contains("approved_profile_hash")) {
        auto previous = record_->at("route"), expected = route_;
        for (const auto* key : {"core_version", "generator_version"}) { previous.erase(key); expected.erase(key); }
        if (previous == expected) return record_->at("approved_profile_hash").get<std::string>();
    }
    return route_.at("profile_config_hash").get<std::string>();
}
std::string LocalPublication::approved_marker_hash() const {
    if (record_ && record_->contains("approved_marker_hash")) {
        auto previous = record_->at("route"), expected = route_;
        for (const auto* key : {"core_version", "generator_version"}) { previous.erase(key); expected.erase(key); }
        if (previous == expected) return record_->at("approved_marker_hash").get<std::string>();
    }
    return route_.at("marker_hash").get<std::string>();
}
void LocalPublication::accept_profile(const std::string& profile_hash) {
    require(snapshot(profile_hash), "invalid-profile-revision", "Use the exact canonical profile revision hash.");
    verify();
    require(core::make_profile(config_.profile, config_.profile_dir).config_hash == profile_hash,
        "profile-revision-changed", "Profile changed before its explicit approval.");
    require(receipt().value("pending_profile_hash", Json("")) == "",
        "refresh-grant-incomplete", "Finish the pending profile publication before explicit approval.");
    if (approved_profile_hash() == profile_hash) return;
    if (!receipt().contains("approved_marker_hash")) receipt()["schema_version"] = "shin.local-publication.v2";
    receipt()["approved_profile_hash"] = profile_hash;
    save();
    verify();
}
void LocalPublication::accept_marker(const std::string& marker_hash) {
    require(hexadecimal(marker_hash, 64), "invalid-marker-revision", "Use the exact lowercase SHA256 marker revision.");
    verify();
    const auto marker = route_.at("marker").get<std::string>();
    require(!marker.empty() && digest(read_text(config_.root / marker, 1024 * 1024, false)) == marker_hash,
        "marker-revision-changed", "Marker changed before its explicit approval.");
    if (receipt().contains("approved_marker_hash") && approved_marker_hash() == marker_hash) return;
    const auto profile_hash = approved_profile_hash();
    if (receipt().value("schema_version", "") != "shin.local-publication.v4")
        receipt()["schema_version"] = "shin.local-publication.v3";
    receipt()["approved_profile_hash"] = profile_hash;
    receipt()["approved_marker_hash"] = marker_hash;
    save();
    verify();
}
void LocalPublication::save() {
    ensure_private_directory(directory_);
    StateStore store(directory_, true);
    store.recover_pending();
    store.save(receipt());
}
void LocalPublication::enable(const std::string& current, const std::string& profile_hash) {
    require(snapshot(current) && snapshot(profile_hash), "refresh-grant-invalid", "Grant requires a verified snapshot and profile revision.");
    (void)directory_identity(config_.out_dir, true);
    if (active()) {
        verify();
        require(approved_profile_hash() == profile_hash, "route-revision-conflict", "Profile revision has not been approved.");
        return;
    }
    require(phase() != "revoking", "refresh-grant-incomplete", "Complete refresh-release before enabling management.");
    if (!record_ || phase() == "revoked") {
        require(!owner(config_.out_dir), "refresh-owner-conflict", "Another home already manages this index.");
        const auto marker_hash = approved_marker_hash();
        auto previous_route = record_ ? record_->at("route") : Json(nullptr), expected_route = route_;
        for (const auto* key : {"core_version", "generator_version"}) {
            if (previous_route.is_object()) previous_route.erase(key);
            expected_route.erase(key);
        }
        const bool keep_marker = record_ && record_->contains("approved_marker_hash") && !marker_hash.empty() && previous_route == expected_route;
        record_ = Json{{"schema_version", "shin.local-publication.v2"}, {"phase", "granting"},
            {"route", route_}, {"token", new_token()}, {"last_snapshot", current}, {"pending_snapshot", ""},
            {"approved_profile_hash", profile_hash}};
        if (keep_marker) {
            (*record_)["schema_version"] = "shin.local-publication.v3";
            (*record_)["approved_marker_hash"] = marker_hash;
        }
        save();
    }
    require(receipt().at("last_snapshot") == current, "refresh-base-changed", "Index changed during publisher setup; review before resuming.");
    try { io::claim_publisher(config_.out_dir, receipt().at("token")); }
    catch (const std::exception&) { throw Error("refresh-owner-conflict", "Cannot claim the index; another owner or incomplete claim requires review."); }
    receipt().at("phase") = "active";
    save();
    require(!reconcile(), "refresh-not-current", "The newly granted publication needs background repair.");
}
void LocalPublication::verify() const {
    require(active() && read_record(directory_) == record_, "refresh-authority-changed", "Publisher permission changed during refresh.");
    require(directory_identity(config_.out_dir, true) == route_.at("index_identity").get<std::string>(),
        "refresh-authority-changed", "Granted index directory was replaced.");
    require(owner(config_.out_dir) == std::optional<std::string>(token()),
        "refresh-owner-conflict", "Index is no longer owned by this publisher; no automatic takeover is allowed.");
}
std::optional<SnapshotRepairBase> LocalPublication::reconcile() {
    verify();
    io::IndexLock writer(config_.out_dir, io::LockMode::exclusive, io::LockTarget::writer, token());
    verify();
    if (io::publication_in_progress(config_.out_dir)) {
        require(receipt().at("pending_snapshot") != "", "refresh-base-changed", "Unattributed interrupted publication requires review.");
        io::recover_snapshot(config_.out_dir, core::kGeneratedDir);
    }
    const auto read = core::lock_index_for_read(config_.out_dir);
    std::string current;
    try { current = core::load_verified_snapshot(config_.out_dir, config_.max_snapshot_bytes, config_.max_file_bytes).snapshot_id; }
    catch (const core::CliError& error) {
        if (!repairable_snapshot_error(error.code())) throw;
        return inspect_snapshot_repair(config_, receipt().at("last_snapshot"));
    }
    require(receipt().at("last_snapshot") == current || receipt().at("pending_snapshot") == current,
        "refresh-base-changed", "Another publisher changed the granted index; automatic replacement is blocked.");
    if (receipt().at("pending_snapshot") != "") committed(current);
    return std::nullopt;
}
void LocalPublication::preparing(const std::string& base, const std::string& candidate, const std::string& profile_hash) {
    verify();
    require(receipt().at("last_snapshot") == base && snapshot(candidate), "refresh-base-changed", "Publication base differs from the grant.");
    if (!profile_hash.empty()) {
        require(snapshot(profile_hash) && core::make_profile(config_.profile, config_.profile_dir).config_hash == profile_hash,
            "profile-revision-changed", "Qualified profile changed before publication.");
        const auto marker = approved_marker_hash();
        receipt()["schema_version"] = "shin.local-publication.v4";
        receipt()["approved_marker_hash"] = marker;
        receipt()["pending_profile_hash"] = profile_hash;
    }
    receipt().at("pending_snapshot") = candidate;
    save();
}
void LocalPublication::committed(const std::string& current) {
    verify();
    if (receipt().contains("pending_profile_hash")) {
        if (receipt().at("pending_snapshot") == current && receipt().at("pending_profile_hash") != "")
            receipt()["approved_profile_hash"] = receipt().at("pending_profile_hash");
        receipt()["pending_profile_hash"] = "";
    }
    receipt().at("last_snapshot") = current;
    receipt().at("pending_snapshot") = "";
    save();
}

Json revoke_local_publication(const fs::path& workspace, const Json& binding) {
    const auto directory = workspace / "publication";
    auto record = read_record(directory);
    if (!record || record->at("phase") == "revoked") return {{"ok", true}, {"publication", "read-only"}, {"data_preserved", true}};
    const auto& route = record->at("route");
    require(route.at("root") == binding.at("root") && route.at("root_identity") == binding.at("root_identity") &&
        route.at("root_identity") == directory_identity(binding.at("root").get<std::string>()) &&
        route.at("workspace_identity") == directory_identity(workspace, true) &&
        route.at("home_identity") == directory_identity(workspace.parent_path().parent_path(), true),
        "refresh-grant-changed", "Publisher workspace identity changed; release is refused.");
    const fs::path output = route.at("output").get<std::string>();
    require(directory_identity(output, true) == route.at("index_identity").get<std::string>(), "refresh-grant-changed", "Index directory changed before release.");
    guard_project_storage(workspace.parent_path().parent_path(),binding.at("root").get<std::string>());
    StateStore store(directory, true);
    store.recover_pending();
    record->at("phase") = "revoking";
    store.save(*record);
    if (io::publication_in_progress(output)) {
        require(record->at("pending_snapshot") != "", "refresh-base-changed", "Unattributed publication requires explicit repair before release.");
        io::IndexLock writer(output, io::LockMode::exclusive, io::LockTarget::writer, record->at("token"));
        io::recover_snapshot(output, core::kGeneratedDir);
    }
    try { io::release_publisher(output, record->at("token")); }
    catch (const std::exception&) { throw Error("refresh-owner-conflict", "Ownership cannot be released; the recorded intent is retained."); }
    record->at("phase") = "revoked";
    store.save(*record);
    return {{"ok", true}, {"publication", "read-only"}, {"data_preserved", true}};
}

namespace {
constexpr std::uintmax_t storage_quota = 8ULL * 1024 * 1024 * 1024;
constexpr std::uintmax_t storage_reserve = 2ULL * 1024 * 1024 * 1024;
bool storage_overlap(const fs::path& a, const fs::path& b) {
    const auto pair = std::mismatch(a.begin(), a.end(), b.begin(), b.end());
    return pair.first == a.end() || pair.second == b.end();
}
struct StorageProject { fs::path workspace, root, output; Json binding; };
std::vector<StorageProject> storage_projects(const fs::path& home) {
    require(home.is_absolute() && fs::weakly_canonical(home) == home,
        "unsafe-project-storage", "Shin storage must have no symbolic-link components.");
    std::vector<StorageProject> projects;
    if (!candidate_present(home / "projects")) return projects;
    (void)directory_identity(home, true); (void)directory_identity(home / "projects", true);
    unsigned count = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    for (const auto& entry : fs::directory_iterator(home / "projects")) {
        require(++count <= 256 && hexadecimal(entry.path().filename().string(),64),
            "storage-inventory-invalid", "Unknown or excessive project storage; preserve it for review.");
        const auto identity = directory_identity(entry.path(), true);
        StateStore state(entry.path(), false);
        auto binding = state.load();
        while (!binding) {
            // Failed preflight may leave only its lock. It grants no root or
            // cleanup ownership and must not prevent a later explicit init.
            const bool empty = std::all_of(fs::directory_iterator(entry.path()),fs::directory_iterator{},
                [](const auto& file) { return file.path().filename()=="observer.lock"; });
            if (empty) break;
            if (!state.writer_present())
                throw Error("storage-inventory-invalid", "Project storage has no matching binding.");
            require(std::chrono::steady_clock::now()<deadline,"storage-project-busy","Another project is being connected; retry shortly.");
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            binding = state.load();
        }
        if (!binding) continue;
        require(binding->is_object() && binding->contains("root") &&
            binding->value("workspace_identity", "") == identity,
            "storage-inventory-invalid", "Project storage has no matching binding.");
        const fs::path root = binding->at("root").get<std::string>();
        require(root.is_absolute() && root.lexically_normal() == root && root != root.root_path() &&
            digest(root.string()) == entry.path().filename(), "storage-inventory-invalid", "Invalid project storage binding.");
        // Protect both the recorded spelling and its current resolved location.
        require(!storage_overlap(home, root) && !storage_overlap(home, fs::weakly_canonical(root)),
            "unsafe-project-storage", "Shin storage overlaps a project directory.");
        fs::path output = entry.path() / "index";
        if (binding->value("schema_version", "") == "shin.local-route-binding.v1") {
            const auto config = load_config(binding->at("observer_config").get<std::string>());
            require(config.root == root && config.binding_key == binding->at("observer_binding_key").get<std::string>(),
                "storage-binding-changed", "Project settings changed; no cleanup is allowed.");
            output = config.out_dir;
            if (candidate_present(output)) require(directory_identity(output)==binding->at("index_identity").get<std::string>(),
                "storage-binding-changed","External index directory was replaced; it is preserved.");
        } else require(binding->value("schema_version", "") == "shin.local-project-binding.v1",
            "storage-inventory-invalid", "Unknown binding format.");
        projects.push_back({entry.path(),root,output,*binding});
    }
    std::sort(projects.begin(), projects.end(), [](const auto& a, const auto& b) { return a.root < b.root; });
    return projects;
}
void external_storage(const fs::path& path, const std::vector<StorageProject>& projects) {
    require(path.is_absolute() && path.lexically_normal() == path && fs::weakly_canonical(path) == path,
        "unsafe-project-storage", "Storage path has a symbolic link or ambiguous location.");
    for (const auto& project : projects)
        require(!storage_overlap(path, project.root) && !storage_overlap(path, fs::weakly_canonical(project.root)),
            "unsafe-project-storage", "Project directories are never modified by storage management.");
}
Json storage_policy(const fs::path& home) {
    Json policy{{"schema_version","shin.local-storage.v1"},{"auto_clean",false}};
    if (candidate_present(home / "storage")) {
        const auto saved = StateStore(home / "storage", false).load();
        if (saved) {
            require(saved->value("schema_version", "") == "shin.local-storage.v1" &&
                saved->contains("auto_clean") && saved->at("auto_clean").is_boolean(),
                "storage-policy-invalid", "Storage policy is invalid; preserve it for review.");
            policy = *saved;
        }
    }
    return policy;
}
bool storage_contains(const fs::path& parent, const fs::path& child) {
    return std::mismatch(parent.begin(),parent.end(),child.begin(),child.end()).first==parent.end();
}
Json storage_usage(const fs::path& home, const std::vector<StorageProject>& projects, bool details = true) {
    std::vector<fs::path> roots{home};
    for (const auto& project : projects) {
        // Legacy indexes inside projects are neither traversed nor counted.
        if (std::any_of(projects.begin(),projects.end(),[&](const auto& p){return storage_overlap(p.root,project.output);})) continue;
        external_storage(project.output,projects);
        if (std::any_of(roots.begin(),roots.end(),[&](const auto& p){return storage_contains(p,project.output);})) continue;
        std::erase_if(roots,[&](const auto& p){return storage_contains(project.output,p);});
        roots.push_back(project.output);
    }
    std::uintmax_t total=0, shared=0;
    std::vector<std::array<std::uintmax_t,3>> sizes(projects.size());
    for (const auto& root : roots) {
        if (!candidate_present(root)) continue;
        (void)directory_identity(root);
        unsigned count=0;
        for (const auto& entry : fs::recursive_directory_iterator(root)) {
            require(++count<=65536,"storage-inventory-limit","Storage inventory exceeds its bounded limit.");
            const auto status=entry.symlink_status();
            require(!fs::is_symlink(status),"unsafe-project-storage","Storage contains a symbolic link; no cleanup is allowed.");
            if (!fs::is_regular_file(status)) {
                require(fs::is_directory(status) || entry.path().filename()=="rpc.sock",
                    "storage-inventory-invalid","Storage contains an unknown special file.");
                continue;
            }
            const auto bytes=entry.file_size();
            require(total<=std::numeric_limits<std::uintmax_t>::max()-bytes,"storage-inventory-limit","Storage size overflow.");
            total+=bytes;
            if (!details) continue;
            std::size_t owner=projects.size(); unsigned matches=0;
            for (std::size_t i=0;i<projects.size();++i)
                if (storage_contains(projects[i].workspace,entry.path()) || storage_contains(projects[i].output,entry.path())) {
                    owner=i; ++matches;
                }
            // Shared/ambiguous locations count once and are never guessed into a project.
            if (matches!=1) { shared+=bytes; continue; }
            const auto& project=projects[owner];
            const bool cache=storage_contains(project.workspace/"source-cache",entry.path()) ||
                storage_contains(project.workspace/"observer/source-cache",entry.path());
            const auto kind=cache ? 1 : storage_contains(project.output,entry.path()) ? 0 : 2;
            sizes[owner][kind]+=bytes;
        }
    }
    Json rows=Json::object();
    for (std::size_t i=0;details && i<projects.size();++i) {
        const auto& size=sizes[i];
        rows[projects[i].root.string()]={{"used_bytes",size[0]+size[1]+size[2]},
            {"index_bytes",size[0]},{"cache_bytes",size[1]},{"other_bytes",size[2]}};
    }
    return {{"used_bytes",total},{"shared_bytes",shared},{"project_bytes",total-shared},
        {"size_kind","logical-files"},{"sampled_at_ms",now_ms()},{"by_root",std::move(rows)}};
}
std::uintmax_t total_storage_bytes(const fs::path& home, const std::vector<StorageProject>& projects) {
    return storage_usage(home,projects,false).at("used_bytes").get<std::uintmax_t>();
}
void verify_cache_owner(const std::string& bytes, const StorageProject& project, const std::string& key) {
    require(bytes.size() >= 100 && bytes.starts_with("SQLite format 3"), "storage-cache-unverified", "Cache format is not recognized.");
    sqlite3* raw = nullptr;
    const auto opened = sqlite3_open(":memory:",&raw);
    std::unique_ptr<sqlite3,decltype(&sqlite3_close)> db(raw,sqlite3_close);
    require(opened == SQLITE_OK, "storage-cache-unverified", "Cache cannot be verified.");
    sqlite3_db_config(raw,SQLITE_DBCONFIG_TRUSTED_SCHEMA,0,nullptr);
    require(sqlite3_deserialize(raw,"main",reinterpret_cast<unsigned char*>(const_cast<char*>(bytes.data())),
        static_cast<sqlite3_int64>(bytes.size()),static_cast<sqlite3_int64>(bytes.size()),SQLITE_DESERIALIZE_READONLY)==SQLITE_OK,
        "storage-cache-unverified", "Cache cannot be verified.");
    int steps = 0;
    sqlite3_progress_handler(raw,1000,[](void* p)->int { return ++*static_cast<int*>(p)>100 ? 1 : 0; },&steps);
    sqlite3_stmt* check = nullptr;
    require(sqlite3_prepare_v2(raw,"PRAGMA quick_check(1)",-1,&check,nullptr)==SQLITE_OK,"storage-cache-unverified","Cache integrity cannot be checked.");
    std::unique_ptr<sqlite3_stmt,decltype(&sqlite3_finalize)> checked_cache(check,sqlite3_finalize);
    require(sqlite3_step(check)==SQLITE_ROW && std::string(reinterpret_cast<const char*>(sqlite3_column_text(check,0)))=="ok" &&
        sqlite3_step(check)==SQLITE_DONE,"storage-cache-unverified","Cache integrity failed.");
    sqlite3_stmt* schema = nullptr;
    require(sqlite3_prepare_v2(raw,"SELECT name FROM sqlite_master WHERE name NOT LIKE 'sqlite_%'",-1,&schema,nullptr)==SQLITE_OK,
        "storage-cache-unverified","Cache schema cannot be verified.");
    std::unique_ptr<sqlite3_stmt,decltype(&sqlite3_finalize)> checked_schema(schema,sqlite3_finalize);
    std::set<std::string> tables; int schema_step;
    while ((schema_step=sqlite3_step(schema))==SQLITE_ROW) {
        require(tables.size()<14 && sqlite3_column_type(schema,0)==SQLITE_TEXT,"storage-cache-unverified","Cache schema is invalid.");
        tables.insert(reinterpret_cast<const char*>(sqlite3_column_text(schema,0)));
    }
    const std::set<std::string> legacy_tables{"cache_meta","chunks","chunks_config","chunks_content","chunks_data","chunks_docsize","chunks_idx",
        "query_text","query_text_config","query_text_content","query_text_data","query_text_docsize","query_text_idx","query_vocab"};
    const std::set<std::string> current_tables{"cache_meta","chunks","chunks_config","chunks_content","chunks_data","chunks_docsize","chunks_idx"};
    require(schema_step==SQLITE_DONE && (tables==legacy_tables || tables==current_tables),
        "storage-cache-unverified","Cache contains unknown tables; it is preserved.");
    sqlite3_stmt* query = nullptr;
    const auto prepared = sqlite3_prepare_v2(raw,"SELECT payload FROM cache_meta WHERE id=0",-1,&query,nullptr);
    std::unique_ptr<sqlite3_stmt,decltype(&sqlite3_finalize)> statement(query,sqlite3_finalize);
    require(prepared == SQLITE_OK && sqlite3_step(query)==SQLITE_ROW && sqlite3_column_type(query,0)==SQLITE_TEXT &&
        sqlite3_column_bytes(query,0) <= static_cast<int>(kMaximumStateBytes),"storage-cache-unverified","Cache ownership is unavailable.");
    const auto payload = parse_json(reinterpret_cast<const char*>(sqlite3_column_text(query,0)));
    require(payload.size()==7 && payload.at("files").is_number_unsigned() && payload.at("files").get<std::uintmax_t>()<=65536 &&
        payload.at("chunks").is_number_unsigned() && payload.at("chunks").get<std::uintmax_t>()<=65536 &&
        payload.at("source_bytes").get<std::uintmax_t>()<=64ULL*1024*1024 &&
        payload.at("indexed_text_bytes").get<std::uintmax_t>()<=core::SourceCache::maximum_bytes,
        "storage-cache-unverified","Cache metadata is outside its format bounds.");
    const auto& binding = payload.at("binding"); const auto& identity = binding.at("identity");
    const bool known_format = (binding.at("format") == "shin.source-cache.v2" && tables == legacy_tables) ||
                             (binding.at("format") == "shin.source-cache.v3" && tables == current_tables);
    require(known_format && identity.at("root") == project.root.generic_string() &&
        identity.at("index") == project.output.generic_string() && core::fnv1a64(identity.dump()).substr(8)==key,
        "storage-cache-unverified", "Cache belongs to a different project or index.");
}
struct StoragePlan { Json response; Json files = Json::array(); std::vector<StorageProject> projects; };
StoragePlan plan_storage(const fs::path& home, const std::optional<fs::path>& selected) {
    StoragePlan plan; plan.projects = storage_projects(home);
    const auto policy = storage_policy(home);
    const auto usage = storage_usage(home,plan.projects);
    plan.response = {{"ok",true},{"schema_version","shin.local-storage-status.v1"},
        {"used_bytes",usage.at("used_bytes")},{"shared_bytes",usage.at("shared_bytes")},
        {"project_bytes",usage.at("project_bytes")},{"size_kind",usage.at("size_kind")},
        {"sampled_at_ms",usage.at("sampled_at_ms")},{"quota_bytes",storage_quota},
        {"reserve_free_bytes",storage_reserve},{"auto_clean",policy.at("auto_clean")},
        {"reclaimable_bytes",0ULL},{"projects",Json::array()},{"history_preserved",true},{"project_files_mutated",false}};
    for (const auto* key : {"last_cleanup_at_ms","last_cleanup_bytes","last_cleanup_error"})
        if (policy.contains(key)) plan.response[key]=policy.at(key);
    Json identity = Json::array(); bool found = !selected;
    for (const auto& project : plan.projects) {
        identity.push_back({{"workspace",project.workspace.string()},{"root",project.root.string()},
            {"workspace_identity",project.binding.at("workspace_identity")},{"output",project.output.string()},
            {"lifecycle",local_project_lifecycle(project.workspace).revision}});
        if (selected && fs::weakly_canonical(*selected) != project.root) continue;
        found = true;
        const bool detached = local_project_lifecycle(project.workspace).phase == "detached";
        Json row{{"root",project.root.string()},{"detached",detached},{"reclaimable_bytes",0ULL},
            {"reason",detached ? "external-caches-only" : "active-project-preserved"}};
        row.update(usage.at("by_root").at(project.root.string()));
        if (detached) for (const auto& folder : {project.workspace / "source-cache",project.workspace / "observer/source-cache"}) {
            if (!candidate_present(folder)) continue;
            external_storage(folder,plan.projects);
            const auto folder_identity = directory_identity(folder,true);
            unsigned count = 0;
            for (const auto& entry : fs::directory_iterator(folder)) {
                require(++count <= 128,"storage-inventory-limit","Too many cache entries.");
                const auto key = entry.path().stem().string();
                if (entry.path().extension() != ".sqlite" || !hexadecimal(key,16)) continue;
                try {
                    const auto bytes = read_text(entry.path(),core::SourceCache::maximum_bytes,true);
                    verify_cache_owner(bytes,project,key);
                    plan.files.push_back({{"path",entry.path().string()},{"root",project.root.string()},
                        {"workspace",project.workspace.string()},{"directory_identity",folder_identity},
                        {"bytes",bytes.size()},{"hash",digest(bytes)},{"key",key}});
                    row["reclaimable_bytes"] = row["reclaimable_bytes"].get<std::uintmax_t>()+bytes.size();
                } catch (const std::exception&) { row["reason"]="unverified-files-preserved"; }
            }
        }
        plan.response["reclaimable_bytes"] = plan.response["reclaimable_bytes"].get<std::uintmax_t>() + row["reclaimable_bytes"].get<std::uintmax_t>();
        plan.response["projects"].push_back(std::move(row));
    }
    require(found,"storage-project-unknown","Project has no known storage binding.");
    plan.response["revision"] = digest(Json{{"identity",identity},{"files",plan.files},{"policy",policy},
        {"scope",selected ? selected->string() : ""}}.dump());
    return plan;
}
}

void guard_project_storage(const fs::path& home, const fs::path& root) {
    auto projects = storage_projects(home);
    const auto canonical = fs::canonical(root);
    require(!storage_overlap(home,canonical),"unsafe-project-storage","Shin storage cannot be inside a project.");
    for (const auto& project : projects)
        require(!storage_overlap(canonical,project.output),"unsafe-project-storage","The selected project contains existing Shin storage.");
    projects.push_back({{},canonical,{},Json::object()});
    external_storage(home,projects);
}

void guard_external_storage(const fs::path& home, const fs::path& root, const fs::path& output) {
    auto projects=storage_projects(home); projects.push_back({{},fs::canonical(root),{},Json::object()});
    external_storage(output,projects);
}

Json local_storage(const fs::path& input, const std::optional<fs::path>& root,
                   const std::string& revision, const std::optional<bool>& automatic) {
    const auto home = fs::absolute(input).lexically_normal();
    const auto projects = storage_projects(home); external_storage(home,projects);
    if (revision.empty() && !automatic) return plan_storage(home,root).response;
    (void)directory_identity(home,true);
    ensure_private_directory(home / "storage");
    StateStore operation(home / "storage",true);
    auto plan = plan_storage(home,root);
    if (automatic) {
        auto policy = storage_policy(home); policy["auto_clean"] = *automatic;
        operation.save(policy); plan.response["auto_clean"] = *automatic;
        return plan.response;
    }
    require(plan.response.at("revision") == revision,"storage-plan-changed","Storage changed; review the cleanup preview again.");
    std::uintmax_t reclaimed = 0; std::string current_path;
    plan.response["actions"] = Json::array();
    for (const auto& file : plan.files) plan.response["actions"].push_back({{"path",file.at("path")},{"bytes",file.at("bytes")},{"applied",false}});
    try {
    for (const auto& project : plan.projects) {
        if (root && fs::weakly_canonical(*root) != project.root) continue;
        if (local_project_lifecycle(project.workspace).phase != "detached") continue;
        StateStore binding(project.workspace,true,project.binding.at("workspace_identity"),false);
        require(binding.load() == std::optional<Json>(project.binding) && local_project_lifecycle(project.workspace).phase == "detached",
            "storage-binding-changed","Project was reconnected; cleanup stopped.");
        if (candidate_present(project.workspace / "watch"))
            require(!StateStore(project.workspace / "watch",false).writer_present(),"storage-project-busy","Project controller is still running.");
        const auto enrolled = candidate_present(home / "registry") ? registered_local_projects(home) : std::vector<LocalProjectRequest>{};
        require(std::none_of(enrolled.begin(),enrolled.end(),[&](const auto& p){return p.root==project.root;}),
            "storage-project-busy","Project is still enrolled.");
        for (const auto& file : plan.files) {
            if (file.at("root") != project.root.string()) continue;
            const fs::path path = file.at("path").get<std::string>(); current_path=path.string();
            external_storage(path,storage_projects(home));
            require(directory_identity(path.parent_path(),true)==file.at("directory_identity").get<std::string>(),
                "storage-plan-changed","Cache directory changed.");
            // The Core slot lock excludes both current and one-shot cache writers.
            core::SourceCache slot(path.parent_path(),file.at("key"),false);
            const auto bytes = slot.load();
            if (!bytes || digest(*bytes)!=file.at("hash").get<std::string>())
                throw Error("storage-plan-changed","Cache changed since preview.");
            verify_cache_owner(*bytes,project,file.at("key"));
            remove_user_archive(path,*bytes,core::SourceCache::maximum_bytes);
            reclaimed += bytes->size();
            for (auto& action : plan.response["actions"]) if (action.at("path")==path.string()) action["applied"]=true;
        }
    }
    } catch (const Error& error) {
        plan.response["ok"] = false; plan.response["error"] = error_json(error);
    } catch (const core::CliError& error) {
        plan.response["ok"] = false; plan.response["error"] = {{"code",error.code()},{"message",error.what()}};
    } catch (const std::exception&) {
        plan.response["ok"] = false; plan.response["error"] = {{"code","storage-unavailable"},{"message","Cleanup stopped; completed removals are reported."}};
    }
    if (!plan.response.at("ok").get<bool>()) for (auto& action : plan.response["actions"])
        if (action.at("path")==current_path && !action.at("applied").get<bool>()) action["error"]=plan.response.at("error").at("code");
    plan.response["reclaimed_bytes"] = reclaimed;
    plan.response["reclaimable_bytes"] = plan.response["reclaimable_bytes"].get<std::uintmax_t>() - reclaimed;
    return plan.response;
}

void verify_storage_capacity(std::uintmax_t used, std::uintmax_t requested, std::uintmax_t available) {
    require(used <= storage_quota && requested <= storage_quota-used,
        "storage-quota-exceeded","Shin storage budget would be exceeded; disconnect and clear unused caches.");
    require(available >= storage_reserve && requested <= available-storage_reserve,
        "storage-free-space-low","Insufficient free disk space for a safe update; current data is preserved.");
}

LocalStorageReservation::LocalStorageReservation(const fs::path& input, const fs::path& output, std::uintmax_t bytes,
                                                 const std::optional<fs::path>& new_root) {
    const auto home = fs::absolute(input).lexically_normal();
    const auto projects = storage_projects(home); external_storage(home,projects); external_storage(output,projects);
    if (!candidate_present(home)) ensure_private_directory(home);
    ensure_private_directory(home / "storage");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    for (;;) {
        try { lock_ = std::make_unique<StateStore>(home / "storage",true); break; }
        catch (const Error& error) {
            if (error.code!="writer-busy" || std::chrono::steady_clock::now()>=deadline) throw;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
    // Another initializer may have committed while this admission was waiting.
    const auto current = storage_projects(home);
    external_storage(home,current); external_storage(output,current);
    if (new_root) { guard_project_storage(home,*new_root); guard_external_storage(home,*new_root,output); }
    const auto used = total_storage_bytes(home,current);

    for (auto target : {home,output}) {
        while (!candidate_present(target)) target = target.parent_path();
        const auto space = fs::space(target);
        verify_storage_capacity(used,bytes,space.available);
    }
}
LocalStorageReservation::~LocalStorageReservation() = default;

void automatic_local_storage(const fs::path& home) {
    std::string error; std::uintmax_t reclaimed = 0; Json actions=Json::array(); std::size_t omitted=0;
    try {
        if (!candidate_present(home / "storage") || !storage_policy(home).at("auto_clean").get<bool>()) return;
        const auto plan = local_storage(home);
        if (plan.at("reclaimable_bytes").get<std::uintmax_t>() != 0) {
            const auto result = local_storage(home,{},plan.at("revision"));
            reclaimed = result.value("reclaimed_bytes",0ULL);
            for (const auto& action : result.at("actions")) { if (actions.size()<32) actions.push_back(action); else ++omitted; }
            if (!result.at("ok").get<bool>()) error=result.at("error").at("code");
        }
    } catch (const Error& failure) { error=failure.code; }
      catch (const std::exception&) { error="storage-unavailable"; }
    try {
        const auto projects=storage_projects(home); external_storage(home,projects);
        StateStore store(home / "storage",true); auto policy=storage_policy(home);
        policy["last_cleanup_at_ms"]=now_ms(); policy["last_cleanup_bytes"]=reclaimed; policy["last_cleanup_error"]=error;
        policy["last_cleanup_actions"]=actions; policy["last_cleanup_actions_omitted"]=omitted;
        store.save(policy);
    } catch (const std::exception&) {
        // No success receipt is fabricated when state cannot be safely written.
        // storage-status exposes the same unsafe/unavailable state to the UI.
        return;
    }
}
}
