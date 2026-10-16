#include "route_access.h"

namespace shin::server {
void select_content_route(pqxx::transaction_base& tx, const std::string& route) {
    tx.exec("SELECT set_config('shin.content_route',$1,true)", pqxx::params{tx, route});
}
namespace {
template<class F> auto reference_query(pqxx::transaction_base& tx, F query) {
    const auto prior = tx.exec("SELECT current_setting('shin.route_refs',true)")[0][0].as<std::string>("");
    tx.exec("SELECT set_config('shin.route_refs','1',true)");
    const auto result = query();
    tx.exec("SELECT set_config('shin.route_refs',$1,true)", pqxx::params{tx, prior});
    return result;
}
}
bool preparation_has_active_route(pqxx::transaction_base& tx, std::uint64_t id) {
    // Artifact safety checks all references, including maintenance-created foreign pins; no payload leaves this helper.
    return reference_query(tx, [&] {
        return !tx.exec("SELECT 1 FROM shin_server.active_routes WHERE preparation_id=$1", pqxx::params{tx, id}).empty();
    });
}
std::int64_t activation_history_size(pqxx::transaction_base& tx) {
    return reference_query(tx, [&] {
        return tx.exec("SELECT count(*) FROM shin_server.activation_receipts")[0][0].as<std::int64_t>();
    });
}
}
