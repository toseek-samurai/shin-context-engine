#include "legacy_project_access.h"
#include <cassert>
#include <iostream>
#include <regex>

namespace {
template<class Action> void denied(Action action, int status, const char* code) {
    bool rejected = false;
    try { action(); }
    catch (const shin::server::ServiceError& error) { rejected = error.status == status && error.code == code; }
    assert(rejected);
}
}
int main() try {
    using namespace shin::server;
    using namespace shin::server::legacy_access;
    const Principal reader{"reader", false, {"alpha"}};
    const Principal admin{"admin", true, {}};
    validate_principal(reader); validate_principal(admin);
    assert(principal_can_access(reader, "alpha"));
    assert(!principal_can_access(reader, "beta"));
    assert(principal_can_access(admin, "beta"));
    require_project_grant(reader, "alpha"); require_admin(admin);
    denied([&] { require_project_grant(reader, "beta"); }, 404, "project-not-found");
    denied([&] { require_admin(reader); }, 403, "forbidden");
    Principal large = reader;
    large.project_ids.resize(128, "alpha"); validate_principal(large);
    large.project_ids.push_back("alpha");
    for (bool privileged : {false, true}) {
        large.admin = privileged;
        denied([&] { validate_principal(large); }, 403, "principal-scope-invalid");
    }
    const std::regex grammar("[a-z][a-z0-9-]{0,62}");
    for (int byte = 0; byte < 256; ++byte) {
        const auto id = std::string("actor-") + static_cast<char>(byte);
        for (bool privileged : {false, true}) for (bool grant : {false, true}) {
            auto actor = reader; actor.admin = privileged;
            (grant ? actor.project_ids[0] : actor.id) = id;
            if (std::regex_match(id, grammar)) validate_principal(actor);
            else denied([&] { validate_principal(actor); }, 403, "principal-scope-invalid");
        }
    }
    for (const auto& id : {std::string(), std::string(64, 'a'), std::string("1alpha"), std::string("Alpha")}) {
        auto actor = admin; actor.id = id;
        denied([&] { validate_principal(actor); }, 403, "principal-scope-invalid");
    }
    auto producer = reader; producer.receipt_producer = true;
    assert(!principal_can_access(producer, "beta"));
    denied([&] { require_admin(producer); }, 403, "forbidden");
    std::cout << "legacy grant/role bounds and identifier equivalence passed\n";
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
