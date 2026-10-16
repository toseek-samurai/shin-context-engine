#include "shin/server/preparation_service.h"
#include "shin/server/route_files.h"
#include "profile_quality.h"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <fcntl.h>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace shin::context {
SuiteEvaluation evaluate_pack_cases_with_observation_barrier(RetrievalSession& session);
}

namespace {
using namespace shin::server;
namespace fs = std::filesystem;
constexpr std::string_view kProject = "visible";
constexpr std::string_view kRoute = "visible-retrieval";
constexpr std::uintmax_t kMaxSourceBytes = 1024 * 1024;

void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

bool below(const fs::path& child, const fs::path& parent) {
    const auto relative = child.lexically_relative(parent);
    return !relative.empty() && relative != "." && !relative.is_absolute() && *relative.begin() != "..";
}

class Descriptor {
public:
    explicit Descriptor(const fs::path& path)
        : value_(::open(path.c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK)) {}
    ~Descriptor() { if (value_ >= 0) ::close(value_); }
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
    int get() const { return value_; }
private:
    int value_;
};

class SourceMutation;
SourceMutation* observation_source = nullptr;

class SourceMutation {
public:
    explicit SourceMutation(const fs::path& source) : path_(source), file_(source) {
        check(file_.get() >= 0 && ::fstat(file_.get(), &identity_) == 0 && S_ISREG(identity_.st_mode) &&
                  identity_.st_uid == ::geteuid() && identity_.st_nlink == 1 && identity_.st_size >= 0 &&
                  static_cast<std::uintmax_t>(identity_.st_size) <= kMaxSourceBytes,
              "source is not an owned single-link bounded regular fixture file");
        original_ = read_route_file(path_, kMaxSourceBytes);
        verify_identity();
        check(original_.size() == static_cast<std::size_t>(identity_.st_size), "fixture changed while opening");
        check(observation_source == nullptr, "another observation phase is already installed");
        observation_source = this;
    }
    ~SourceMutation() {
        observation_source = nullptr;
        if (dirty_ && !truncate_original())
            std::cerr << "fixture source restoration failed\n";
    }
    SourceMutation(const SourceMutation&) = delete;
    SourceMutation& operator=(const SourceMutation&) = delete;

    void arm() {
        check(!armed_ && !dirty_ && read_route_file(path_, kMaxSourceBytes) == original_,
              "observation phase requires restored source");
        armed_ = true;
    }

    void after_quality(const shin::context::SuiteEvaluation& result) {
        if (!armed_) return;
        check(!dirty_, "observation phase executed more than once");
        check(result.total == 1 && result.passed == 1 && result.failed == 0 &&
                  result.query_total == 0 && result.query_passed == 0 && result.failed_case_ids.empty(),
              "real Pack-eval did not pass before the source mutation");
        verify_identity();
        check(read_route_file(path_, kMaxSourceBytes) == original_, "source drifted before quality finished");
        // Append preserves the original prefix; even a partial write is undone by RAII truncation.
        constexpr std::string_view suffix = "\npreflight-mid-quality-drift\n";
        dirty_ = true;
        std::size_t offset = 0;
        while (offset < suffix.size()) {
            const auto count = ::pwrite(file_.get(), suffix.data() + offset, suffix.size() - offset,
                                        identity_.st_size + static_cast<off_t>(offset));
            if (count < 0 && errno == EINTR) continue;
            check(count > 0, "fixture source mutation failed");
            offset += static_cast<std::size_t>(count);
        }
        ++mutations_;
    }

    void restore() {
        check(armed_ && dirty_, "quality observation never reached the mutation barrier");
        check(truncate_original(), "fixture source restoration failed");
        verify_identity();
        check(read_route_file(path_, kMaxSourceBytes) == original_, "fixture source was not restored exactly");
        dirty_ = false;
        armed_ = false;
    }
    std::size_t mutations() const { return mutations_; }

private:
    void verify_identity() const {
        struct stat current{};
        check(fs::canonical(path_) == path_ && ::lstat(path_.c_str(), &current) == 0 &&
                  S_ISREG(current.st_mode) && current.st_dev == identity_.st_dev && current.st_ino == identity_.st_ino &&
                  current.st_nlink == 1 && current.st_uid == identity_.st_uid && current.st_mode == identity_.st_mode,
              "fixture source identity changed");
    }
    bool truncate_original() const noexcept {
        int result;
        do {
            result = ::ftruncate(file_.get(), static_cast<off_t>(original_.size()));
        } while (result < 0 && errno == EINTR);
        return result == 0;
    }
    fs::path path_;
    Descriptor file_;
    struct stat identity_{};
    std::string original_;
    bool armed_ = false;
    bool dirty_ = false;
    std::size_t mutations_ = 0;
};

void check_quality(const Json& report) {
    check(report.at("profile_check") == Json{{"state", "passed"}, {"errors", 0}, {"warnings", 0}} &&
              report.at("project_quality_assessed") == true && report.at("quality_passed") == true,
          "candidate quality did not complete successfully");
    for (const auto* name : {"eval", "pack_eval"}) {
        const auto& suite = report.at("quality").at(name);
        const auto queries = std::string_view(name) == "eval" ? 1 : 0;
        check(suite.at("state") == "passed" && suite.at("total") == 1 && suite.at("passed") == 1 &&
                  suite.at("failed") == 0 && suite.at("query_total") == queries && suite.at("query_passed") == queries,
              "candidate quality counters changed");
    }
}

Json run(const fs::path& config_path, std::string_view id_text, const fs::path& source) {
    check(config_path.is_absolute() && fs::canonical(config_path) == config_path &&
              config_path.filename() == "runtime.json" && config_path.parent_path().filename() == "config",
          "probe requires the explicit development runtime fixture configuration");
    std::uint64_t id = 0;
    const auto parsed = std::from_chars(id_text.data(), id_text.data() + id_text.size(), id);
    check(parsed.ec == std::errc{} && parsed.ptr == id_text.data() + id_text.size() && id > 0 &&
              id <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()),
          "probe requires a positive preparation identity");
    const Config config = load_config(config_path);
    check(config.database.expected_database == "shin_server_dev" && config.database.runtime_role == "shin_runtime",
          "probe refuses a non-development database configuration");
    const RouteCatalog catalog{config};
    const auto& binding = catalog.find(std::string(kProject), std::string(kRoute));
    const fs::path root = binding.route.root;
    const auto fixture_root = config_path.parent_path().parent_path() / "fixtures" / "roots" / kProject;
    check(root == fixture_root && fs::canonical(root) == root && binding.route.profile == "preflight-passing" &&
              source.is_absolute() && fs::canonical(source) == source && below(source, root) &&
              std::any_of(config.allowed_roots.begin(), config.allowed_roots.end(), [&](const auto& allowed) {
                  return fs::canonical(allowed) == allowed && (root == allowed || below(root, allowed));
              }),
          "source is outside the allowlisted development preflight route");
    const auto admin = std::find_if(config.credentials.begin(), config.credentials.end(),
                                    [](const auto& credential) { return credential.principal.admin; });
    check(admin != config.credentials.end(), "configured administrator is unavailable");
    const auto& principal = admin->principal;
    Store store{config.database};
    const auto input = store.preparation_inspection_input(principal, id);
    check(input.at("job").at("project_id") == kProject && input.at("job").at("route_id") == kRoute &&
              input.at("job").at("state") == "prepared" && input.at("retention").is_null(),
          "probe requires an already prepared unretained fixture candidate");
    catalog.verify(binding, store.get_workspace(principal, std::string(kProject), binding.route.workspace_id));
    const auto active = store.active_route(principal, std::string(kProject), std::string(kRoute));
    check(active.at("revision") == input.at("expected_route_revision"), "candidate base is not current");

    // Construction opens no worker. Never call start() in this observation-only probe.
    PreparationService service{config, catalog, store};
    SourceMutation mutation{source};
    mutation.arm();
    const auto report = service.preflight(principal, id);
    check_quality(report);
    check(report.at("checks").at("source") == "drifted" && report.at("candidate_verified") == false &&
              report.at("preflight_passed") == false && report.at("mutations_performed") == false &&
              report.at("reason_codes") == Json::array({"candidate-source-drifted"}) && mutation.mutations() == 1,
          "preflight accepted source changed after real quality evaluation");
    mutation.restore();

    const auto restored = service.preflight(principal, id);
    check_quality(restored);
    check(restored.at("checks").at("source") == "current" && restored.at("candidate_verified") == true &&
              restored.at("preflight_passed") == true && restored.at("reason_codes").empty() && mutation.mutations() == 1,
          "restored candidate did not pass an unmodified control preflight");
    mutation.arm();
    std::string activation_error;
    try {
        (void)service.activate(principal, std::string(kProject), std::string(kRoute),
                               Json{{"preparation_id", id}, {"expected_revision", active.at("revision")}});
    } catch (const ServiceError& error) {
        activation_error = error.code;
    }
    check(activation_error == "activation-preflight-failed" && mutation.mutations() == 2,
          "activation did not reject source drift after real quality evaluation");
    mutation.restore();
    check(store.active_route(principal, std::string(kProject), std::string(kRoute)) == active &&
              store.preparation_inspection_input(principal, id) == input,
          "observation probe changed the active generation or candidate ledger");
    return {{"ok", true}, {"preflight", report}, {"activation_error", activation_error},
            {"barrier_mutations", mutation.mutations()}, {"source_restored", true}, {"restored_preflight_passed", true}};
}
} // namespace

namespace shin::context {
SuiteEvaluation evaluate_pack_cases_with_observation_barrier(RetrievalSession& session) {
    const auto result = evaluate_pack_cases(session);
    if (observation_source != nullptr) observation_source->after_quality(result);
    return result;
}
} // namespace shin::context

int main(int argc, char** argv) {
    if (argc != 4) {
        std::cout << "{\"ok\":false}\n";
        return 2;
    }
    try {
        std::cout << run(fs::path{argv[1]}, argv[2], fs::path{argv[3]}).dump() << '\n';
        return 0;
    } catch (...) {
        // Do not expose database diagnostics, credentials, or private fixture paths.
        std::cout << "{\"ok\":false}\n";
        return 1;
    }
}
