#include "shin/server/knowledge_health.h"

#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {
using namespace shin::server;

void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

Json generation() {
    return {{"project_id", "alpha"}, {"route_id", "source"}, {"workspace_id", "workspace"},
        {"source_host_id", "local"}, {"profile_id", "docs"}, {"route_fingerprint", "fingerprint"},
        {"preparation_id", 42}, {"snapshot_id", "snapshot"}, {"revision", 17}};
}

void independent_selection_revisions_preserve_source_identity() {
    const auto observed = generation();
    auto publication = observed;
    publication["publication_id"] = "shared";
    publication["revision"] = 3;
    check(knowledge_same_source_generation(observed, publication),
          "publication revision was confused with the route assessment revision");
    check(observed.at("revision") == 17 && publication.at("revision") == 3,
          "source comparison mutated selection identities");
}

void every_source_identity_field_is_required() {
    const auto original = generation();
    for (const auto* field : {"project_id", "route_id", "workspace_id", "source_host_id", "profile_id",
                             "route_fingerprint", "preparation_id", "snapshot_id"}) {
        auto altered = original;
        altered[field] = field == std::string_view("preparation_id") ? Json(43) : Json("different");
        check(!knowledge_same_source_generation(original, altered), "different source provenance was accepted");
        altered.erase(field);
        check(!knowledge_same_source_generation(original, altered), "missing selected provenance was accepted");
        check(!knowledge_same_source_generation(altered, original), "missing observed provenance was accepted");
        altered[field] = nullptr;
        check(!knowledge_same_source_generation(altered, altered), "matching null provenance was promoted");
    }
    check(!knowledge_same_source_generation(nullptr, original), "absent assessment was promoted");
    check(!knowledge_same_source_generation(original, Json::array()), "non-object selection was accepted");
}
} // namespace

int main() {
    try {
        independent_selection_revisions_preserve_source_identity();
        every_source_identity_field_is_required();
        std::cout << "knowledge generation: 2 checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
