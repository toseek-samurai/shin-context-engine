#include "shin/server/http.h"
#include "shin/server/store.h"
#include "shin/server/version.h"
#include "shin/observer_protocol.h"

#include <iostream>
#include <cstdio>
#include <sys/stat.h>

namespace {
int run(int argc, char** argv) {
    using namespace shin::server;
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--version") {
            std::cout << response_envelope({{"service", "server-foundation"}, {"core_baseline", "1.25.0"},
                                           {"core_version", SHIN_VERSION},
                                           {"observer_available", false}, {"observer_ingestion_available", true},
                                           {"remote_workspace_registration", true}, {"retrieval_available", true},
                                           {"preparation_jobs_available", true}, {"activation_available", true},
                                           {"workspace_registry_available", true}, {"git_required", false},
                                           {"candidate_inspection_available", true},
                                           {"candidate_retention_available", true},
                                           {"observer_reclamation_available", true},
                                           {"observer_unconfirmed_discard_available", true},
                                           {"observer_identity_retirement_available", true},
                                           {"retrieval_routing", "managed-generation-config-v10"},
                                           {"knowledge_maintenance_available", true},
                                           {"session_bootstrap_available", true},
                                           {"contracts", {{"configuration_schema", kServiceConfigSchema},
                                               {"recovery_configuration_schema", kRecoveryConfigSchema},
                                               {"observer_identity_schema", shin::observer_protocol::kIdentitySchema},
                                               {"request_identity_header", shin::observer_protocol::kRequestIdentityHeader}}}}).dump() << '\n';
            return 0;
        }
        if (argc == 6 && (std::string_view(argv[1]) == "recovery-plan" || std::string_view(argv[1]) == "recovery-apply" ||
                         std::string_view(argv[1]) == "observer-retirement-plan" || std::string_view(argv[1]) == "observer-retirement-apply") &&
            std::string_view(argv[2]) == "--config" && std::string_view(argv[4]) == "--input") {
            const auto database = load_recovery_config(argv[3]);
            const auto input = load_private_json(argv[5], 16384);
            const std::string_view command(argv[1]);
            const auto mode = command.starts_with("observer-retirement-") ?
                IdentityMaintenance::observer_retirement : IdentityMaintenance::recovery;
            std::cout << response_envelope(Store::recover(database, input, command.ends_with("-apply"), mode)).dump() << '\n';
            return 0;
        }
        if (argc != 4 || std::string_view(argv[2]) != "--config")
            throw ServiceError(400, "invalid-arguments", "Use --version, check-config/migrate/serve --config FILE, or recovery-plan/recovery-apply/observer-retirement-plan/observer-retirement-apply --config FILE --input FILE.");
        const std::string_view command(argv[1]);
        if (command != "migrate" && command != "serve" && command != "check-config")
            throw ServiceError(400, "invalid-arguments", "Unsupported command.");
        const auto config = load_config(argv[3]);
        if (command == "check-config") {
            std::cout << response_envelope({{"configuration_schema", kServiceConfigSchema},
                {"valid", true}, {"read_only", true}, {"database_contacted", false},
                {"source_mutated", false}}).dump() << '\n';
            return 0;
        }
        if (command == "serve") return serve(config);
        Store::migrate(config.database);
        std::cout << response_envelope({{"migration", "current"}}).dump() << '\n';
        return 0;
    } catch (const ServiceError& error) {
        std::cerr << error_envelope(error).dump() << '\n';
        return 1;
    } catch (...) {
        std::cerr << error_envelope(ServiceError(500, "internal-error", "The operation could not be completed.")).dump() << '\n';
        return 1;
    }
}
} // namespace

int main(int argc, char** argv) noexcept {
    // Set once before any worker starts; managed state must not inherit a permissive shell umask.
    ::umask(0077);
    try {
        return run(argc, argv);
    } catch (...) {
        std::fputs("shin-server: fatal failure while reporting an error\n", stderr);
        return 1;
    }
}
