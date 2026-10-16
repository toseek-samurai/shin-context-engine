#include "shin/server/http.h"
#include "shin/server/store.h"
#include "shin/server/retrieval_service.h"
#include "shin/server/preparation_service.h"
#include "shin/server/publication_service.h"
#include "shin/server/knowledge_service.h"
#include "shin/server/continuity_service.h"
#include "shin/server/journal_writer.h"
#include "shin/server/version.h"
#include "shin/server/observer_ingestion.h"
#include "shin/observer_protocol.h"

#include <httplib.h>
#include <algorithm>
#include <charconv>
#include <csignal>
#include <iostream>
#include <limits>
#include <pthread.h>
#include <thread>

namespace shin::server {
namespace {

Json base_envelope(bool ok) {
    return {{"schema_version", "shin.server-response.v1"}, {"tool", "shin-server"},
            {"version", kServerVersion}, {"stability", "development"}, {"ok", ok}};
}

void send(httplib::Response& response, Json value, int status = 200) {
    response.status = status;
    response.set_header("Cache-Control", "no-store");
    response.set_header("X-Content-Type-Options", "nosniff");
    response.set_header("Content-Security-Policy", "default-src 'none'; frame-ancestors 'none'");
    response.set_content(value.dump(), "application/json");
}

void fail(httplib::Response& response, const ServiceError& error) {
    if (error.status == 401) response.set_header("WWW-Authenticate", "Bearer");
    send(response, error_envelope(error), error.status);
}

const Principal& principal(const httplib::Response& response) {
    const auto* actor = response.user_data.get<Principal>("principal");
    if (!actor) throw ServiceError(401, "unauthorized", "Authentication is required.");
    return *actor;
}

bool device_registration(const httplib::Request& request) {
    return request.method == "POST" && request.path == "/api/v1/identity/device-session";
}

void authenticate_request(const Config& config, OidcProvider* provider, Store& store,
                          const httplib::Request& request, httplib::Response& response) {
    const auto auth = request.get_header_value("Authorization");
    if (request.get_header_value_count("Authorization") != 1 || !auth.starts_with("Bearer "))
        throw ServiceError(401, "unauthorized", "Authentication is required.");
    const auto token = std::string_view(auth).substr(7);
    if (!device_registration(request)) {
        // Exact static credentials retain their maintenance/test role. An invalid
        // JWT never falls back to an account-only directory lookup.
        try {
            response.user_data.set("principal", authenticate(config, token));
            return;
        } catch (const ServiceError& error) {
            if (error.status != 401) throw;
        }
    }
    if (!provider) throw ServiceError(401, "unauthorized", "Authentication is required.");
    const auto identity = provider->verify(token);
    if (device_registration(request)) {
        response.user_data.set("verified_identity", identity);
        return; // The explicit registration transaction checks enabled account/binding.
    }
    auto actor = store.identity_resolve_device(identity);
    if (!request.path.starts_with("/api/v1/personal/") &&
        !request.path.starts_with("/api/v1/identity/") && request.path != "/health/ready")
        throw ServiceError(403, "personal-route-required", "Use a personal account route.");
    response.user_data.set("principal", std::move(actor));
    response.user_data.set("oidc", true);
}

void query_fields(const httplib::Request& request, std::initializer_list<std::string_view> names) {
    const auto separator = request.target.find('?');
    if (separator != std::string::npos && separator + 1 < request.target.size()) {
        const auto raw = std::string_view(request.target).substr(separator + 1);
        // httplib deduplicates identical raw pairs. Refuse that lossy parse and empty pairs.
        const auto fields = 1 + static_cast<std::size_t>(std::count(raw.begin(), raw.end(), '&'));
        if (fields != request.params.size())
            throw ServiceError(400, "invalid-query", "Query parameters are invalid.");
    }
    for (const auto& [key, value] : request.params) {
        (void)value;
        if (std::find(names.begin(), names.end(), key) == names.end() || request.params.count(key) != 1)
            throw ServiceError(400, "invalid-query", "Query parameters are invalid.");
    }
}

std::uint64_t query_number(const httplib::Request& request, const char* name,
                           std::uint64_t default_value, std::uint64_t low, std::uint64_t high) {
    if (!request.has_param(name)) return default_value;
    const auto text = request.get_param_value(name);
    std::uint64_t result = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
    if (text.empty() || parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || result < low || result > high)
        throw ServiceError(400, "invalid-query", "Query parameters are invalid.");
    return result;
}

void request_boundary(const Config& config, const httplib::Request& request) {
    const auto host = request.get_header_value("Host");
    const std::string suffix = ":" + std::to_string(config.port);
    if (request.get_header_value_count("Host") != 1 ||
        (host != "127.0.0.1" + suffix && host != "localhost" + suffix))
        throw ServiceError(400, "host-denied", "Host is not allowed.");
    if (request.has_header("Origin")) {
        const auto origin = request.get_header_value("Origin");
        if (request.get_header_value_count("Origin") != 1 ||
            (origin != "http://127.0.0.1" + suffix && origin != "http://localhost" + suffix))
            throw ServiceError(403, "origin-denied", "Origin is not allowed.");
    }
}

void server_identity_boundary(const Config& config, const httplib::Request& request) {
    if (request.path != "/health/live" && request.path != "/health/ready" && request.path != "/api/v1/observer-identity") {
        if (request.get_header_value_count(observer_protocol::kRequestIdentityHeader) != 1)
            throw ServiceError(400, "server-id-required", "One explicit X-Shin-Server-ID header is required.");
        if (request.get_header_value(observer_protocol::kRequestIdentityHeader) != config.database.expected_server_id)
            throw ServiceError(409, "server-identity-mismatch", "Request belongs to a different Server identity.");
    }
}

std::string idempotency_key(const httplib::Request& request) {
    const auto key = request.get_header_value("Idempotency-Key");
    if (request.get_header_value_count("Idempotency-Key") != 1 || key.empty() || key.size() > 128 ||
        !std::all_of(key.begin(), key.end(), [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                   (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == ':';
        })) throw ServiceError(400, "invalid-idempotency-key", "A valid Idempotency-Key is required.");
    return key;
}

Json request_json(const httplib::Request& request) {
    const auto content_type = request.get_header_value("Content-Type");
    if (request.get_header_value_count("Content-Type") != 1 ||
        (content_type != "application/json" && content_type != "application/json; charset=utf-8"))
        throw ServiceError(415, "unsupported-content-type", "Use application/json.");
    return parse_bounded_json(request.body, 32768);
}

} // namespace

Json response_envelope(Json result) {
    auto envelope = base_envelope(true);
    envelope["result"] = std::move(result);
    return envelope;
}

Json error_envelope(const ServiceError& error) {
    auto envelope = base_envelope(false);
    envelope["error"] = {{"code", error.code}, {"message", error.what()}};
    return envelope;
}

int serve(const Config& config) {
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);
    sigaddset(&signals, SIGUSR1);
    if (pthread_sigmask(SIG_BLOCK, &signals, nullptr) != 0)
        throw ServiceError(500, "signal-error", "Signal setup failed.");

    Store store(config.database);
    const auto oidc = config.oidc ? std::make_unique<OidcProvider>(*config.oidc) : nullptr;
    RouteCatalog catalog(config);
    RetrievalService retrieval(config, catalog);
    PreparationService preparations(config, catalog, store);
    PublicationService publications(config, catalog, store, preparations);
    KnowledgeService knowledge(config, catalog, store, retrieval, preparations);
    ContinuityService continuity(store, retrieval);
    auto journal_database = config.database;
    journal_database.pool_size = 1;
    Store journal_store(std::move(journal_database));
    Principal journal_actor;
    if (config.journal_writer.enabled) {
        const auto found = std::find_if(config.credentials.begin(), config.credentials.end(), [&](const Credential& credential) {
            return credential.principal.id == config.journal_writer.principal_id && credential.principal.admin;
        });
        if (found == config.credentials.end())
            throw ServiceError(400, "invalid-journal-writer-policy", "Journal writer principal is unavailable.");
        journal_actor = found->principal;
    }
    JournalWriter journal_writer(config.journal_writer,
        [&](std::uint64_t after) { return retrieval.journal_snapshot(after); },
        [&](const Json& batch) { return retrieval.capture_journal_batch(journal_store, journal_actor, batch); });
    httplib::Server server;
    server.new_task_queue = [&] { return new httplib::ThreadPool(config.workers, config.workers, config.queue_limit); };
    server.set_read_timeout(5).set_write_timeout(5).set_keep_alive_timeout(5);
    server.set_keep_alive_max_count(50).set_payload_max_length(32768);
    server.set_pre_routing_handler([&](const auto& request, auto& response) {
        try {
            request_boundary(config, request);
            if (request.path != "/health/live")
                authenticate_request(config, oidc.get(), store, request, response);
            server_identity_boundary(config, request);
            // Personal handlers authorize their exact scope in the operation's transaction.
            // Do not pay a second pool lease or let them fall through to legacy handlers.
            if (request.path != "/health/live" && !device_registration(request) &&
                !response.user_data.has("oidc") && !request.path.starts_with("/api/v1/personal/"))
                store.identity_request_admission(principal(response),
                    request.path.starts_with("/api/v1/identity/") || request.path == "/health/ready");
            return httplib::Server::HandlerResponse::Unhandled;
        } catch (const ServiceError& error) {
            fail(response, error);
            return httplib::Server::HandlerResponse::Handled;
        }
    });
    server.set_exception_handler([](const auto&, auto& response, std::exception_ptr exception) {
        try {
            std::rethrow_exception(exception);
        } catch (const ServiceError& error) {
            fail(response, error);
        } catch (...) {
            fail(response, ServiceError(500, "internal-error", "The operation could not be completed."));
        }
    });
    server.set_error_handler([](const auto&, auto& response) {
        if (response.body.empty())
            fail(response, ServiceError(response.status, "http-error", "The HTTP request could not be handled."));
        return httplib::Server::HandlerResponse::Handled;
    });
    server.Get("/health/live", [](const auto& request, auto& response) {
        query_fields(request, {});
        send(response, response_envelope({{"live", true}, {"scope", "process"}}));
    });
    server.Get("/health/ready", [&](const auto& request, auto& response) {
        query_fields(request, {});
        send(response, response_envelope(store.readiness()));
    });
    server.Post("/api/v1/identity/device-session", [&](const auto& request, auto& response) {
        query_fields(request, {});
        const auto* identity = response.user_data.template get<VerifiedOidcIdentity>("verified_identity");
        if (!identity) throw ServiceError(401, "unauthorized", "Verified login is required.");
        send(response, response_envelope(store.identity_register_device(*identity, request_json(request))));
    });
    server.Get("/api/v1/identity/account", [&](const auto& request, auto& response) {
        query_fields(request, {});
        send(response, response_envelope(store.identity_account(principal(response))));
    });
    server.Get("/api/v1/identity/accounts", [&](const auto& request, auto& response) {
        query_fields(request, {"after", "limit"});
        send(response, response_envelope(store.identity_accounts(principal(response),
            request.get_param_value("after"), query_number(request, "limit", 20, 1, 100))));
    });
    server.Get(R"(/api/v1/identity/accounts/([a-z][a-z0-9-]{0,62})/external)", [&](const auto& request, auto& response) {
        query_fields(request, {});
        send(response, response_envelope(store.identity_external(principal(response), request.matches[1].str())));
    });
    server.Post(R"(/api/v1/identity/accounts/([a-z][a-z0-9-]{0,62})/external)", [&](const auto& request, auto& response) {
        query_fields(request, {});
        send(response, response_envelope(store.identity_bind_external(principal(response), request.matches[1].str(), request_json(request))));
    });
    server.Post(R"(/api/v1/identity/accounts/([a-z][a-z0-9-]{0,62})/external/state)", [&](const auto& request, auto& response) {
        query_fields(request, {});
        send(response, response_envelope(store.identity_external_state(principal(response), request.matches[1].str(), request_json(request))));
    });
    server.Post("/api/v1/identity/accounts", [&](const auto& request, auto& response) {
        query_fields(request, {});
        const auto& actor = principal(response);
        if (!actor.admin) throw ServiceError(403, "forbidden", "Directory administration is required.");
        const auto input = request_json(request);
        const auto enrolled = input.find("principal_id");
        if (enrolled != input.end() && enrolled->is_string()) {
            const auto found = std::find_if(config.credentials.begin(), config.credentials.end(), [&](const auto& c) {
                return c.principal.id == enrolled->template get<std::string>();
            });
            if ((found == config.credentials.end() && !config.oidc) ||
                (found != config.credentials.end() && found->principal.admin))
                throw ServiceError(400, "invalid-request", "Enroll a non-administrator identity.");
        }
        send(response, response_envelope(store.identity_register_account(actor, input)));
    });
    server.Get(R"(/api/v1/identity/accounts/([a-z][a-z0-9-]{0,62})/devices)", [&](const auto& request, auto& response) {
        query_fields(request, {"after", "limit"});
        send(response, response_envelope(store.identity_devices(principal(response), request.matches[1].str(),
            request.get_param_value("after"), query_number(request, "limit", 20, 1, 100))));
    });
    server.Post(R"(/api/v1/identity/accounts/([a-z][a-z0-9-]{0,62})/devices/([a-z][a-z0-9-]{0,62})/revoke)", [&](const auto& request, auto& response) {
        query_fields(request, {});
        send(response, response_envelope(store.identity_revoke_device(principal(response), request.matches[1].str(),
            request.matches[2].str(), request_json(request))));
    });
    server.Post(R"(/api/v1/identity/accounts/([a-z][a-z0-9-]{0,62})/devices/revoke-all)", [&](const auto& request, auto& response) {
        query_fields(request, {});
        require_fields(request_json(request), {});
        send(response, response_envelope(store.identity_revoke_devices(principal(response), request.matches[1].str())));
    });
    server.Post(R"(/api/v1/identity/accounts/([a-z][a-z0-9-]{0,62})/state)", [&](const auto& request, auto& response) {
        query_fields(request, {});
        send(response, response_envelope(store.identity_account_state(principal(response), request.matches[1].str(), request_json(request))));
    });
    server.Post("/api/v1/identity/tenants", [&](const auto& request, auto& response) {
        query_fields(request, {});
        send(response, response_envelope(store.identity_register_tenant(principal(response), request_json(request))));
    });
    server.Get("/api/v1/identity/tenants", [&](const auto& request, auto& response) {
        query_fields(request, {"after", "limit"});
        send(response, response_envelope(store.identity_tenants(principal(response), request.get_param_value("after"),
            query_number(request, "limit", 20, 1, 100))));
    });
    server.Get(R"(/api/v1/identity/tenants/([a-z][a-z0-9-]{0,62}))", [&](const auto& request, auto& response) {
        query_fields(request, {});
        send(response, response_envelope(store.identity_tenant(principal(response), request.matches[1].str())));
    });
    server.Post(R"(/api/v1/identity/tenants/([a-z][a-z0-9-]{0,62})/members)", [&](const auto& request, auto& response) {
        query_fields(request, {});
        send(response, response_envelope(store.identity_membership(principal(response), request.matches[1].str(), request_json(request))));
    });
    server.Get("/api/v1/telemetry/retrieval", [&](const auto& request, auto& response) {
        query_fields(request, {});
        send(response, response_envelope(retrieval.telemetry_metrics(principal(response))));
    });
    server.Post(R"(/api/v1/identity/tenants/([a-z][a-z0-9-]{0,62})/projects)", [&](const auto& request, auto& response) {
        query_fields(request, {});
        const auto& actor = principal(response);
        if (!actor.admin) throw ServiceError(403, "forbidden", "Ownership administration is required.");
        send(response, response_envelope(store.identity_bind_project(actor, request.matches[1].str(), request_json(request))));
    });
    server.Get(R"(/api/v1/identity/tenants/([a-z][a-z0-9-]{0,62})/projects)", [&](const auto& request, auto& response) {
        query_fields(request, {"after", "limit"});
        send(response, response_envelope(store.identity_projects(principal(response), request.matches[1].str(),
            request.get_param_value("after"), query_number(request, "limit", 20, 1, 100))));
    });
    server.Get(R"(/api/v1/identity/tenants/([a-z][a-z0-9-]{0,62})/projects/([a-z][a-z0-9-]{0,62}))", [&](const auto& request, auto& response) {
        query_fields(request, {});
        send(response, response_envelope(store.identity_project(principal(response), request.matches[1].str(), request.matches[2].str())));
    });
    server.Post(R"(/api/v1/identity/tenants/([a-z][a-z0-9-]{0,62})/projects/([a-z][a-z0-9-]{0,62})/workspaces)", [&](const auto& request, auto& response) {
        query_fields(request, {});
        const auto& actor = principal(response);
        if (!actor.admin) throw ServiceError(403, "forbidden", "Ownership administration is required.");
        send(response, response_envelope(store.identity_bind_workspace(actor, request.matches[1].str(), request.matches[2].str(), request_json(request))));
    });
    server.Get(R"(/api/v1/identity/tenants/([a-z][a-z0-9-]{0,62})/projects/([a-z][a-z0-9-]{0,62})/workspaces)", [&](const auto& request, auto& response) {
        query_fields(request, {"after", "limit"});
        send(response, response_envelope(store.identity_workspaces(principal(response), request.matches[1].str(),
            request.matches[2].str(), request.get_param_value("after"), query_number(request, "limit", 20, 1, 100))));
    });
    server.Get(R"(/api/v1/identity/tenants/([a-z][a-z0-9-]{0,62})/projects/([a-z][a-z0-9-]{0,62})/workspaces/([a-z][a-z0-9-]{0,62}))", [&](const auto& request, auto& response) {
        query_fields(request, {});
        send(response, response_envelope(store.identity_workspace(principal(response), request.matches[1].str(), request.matches[2].str(), request.matches[3].str())));
    });
    server.Get("/api/v1/telemetry/retrieval/requests", [&](const auto& request, auto& response) {
        query_fields(request, {"after", "limit", "instance_id"});
        send(response, response_envelope(retrieval.telemetry_requests(principal(response),
            query_number(request, "after", 0, 0, std::numeric_limits<std::uint64_t>::max()),
            query_number(request, "limit", 20, 1, 32), request.get_param_value("instance_id"))));
    });
    server.Post("/api/v1/telemetry/retrieval/journal/capture", [&](const auto& request, auto& response) {
        const auto& actor = principal(response);
        if (!actor.admin) throw ServiceError(403, "forbidden", "Administrator access is required.");
        query_fields(request, {});
        require_fields(request_json(request), {});
        send(response, response_envelope(retrieval.capture_journal(store, actor)));
    });
    server.Get("/api/v1/telemetry/retrieval/journal/writer", [&](const auto& request, auto& response) {
        if (!principal(response).admin)
            throw ServiceError(403, "forbidden", "Administrator access is required.");
        query_fields(request, {});
        send(response, response_envelope(journal_writer.status()));
    });
    server.Get("/api/v1/telemetry/retrieval/journal", [&](const auto& request, auto& response) {
        query_fields(request, {"project_id", "after", "limit"});
        send(response, response_envelope(store.retrieval_journal(principal(response), request.get_param_value("project_id"),
            query_number(request, "after", 0, 0, INT64_MAX), query_number(request, "limit", 20, 1, 32))));
    });
    server.Get("/api/v1/knowledge-maintenance", [&](const auto& request, auto& response) {
        query_fields(request, {});
        send(response, response_envelope(knowledge.status(principal(response))));
    });
    server.Post("/api/v1/knowledge-maintenance/refresh", [&](const auto& request, auto& response) {
        query_fields(request, {});
        send(response, response_envelope(knowledge.refresh(principal(response), request_json(request))), 202);
    });
    server.Get(R"(/api/v1/projects/([a-z][a-z0-9-]{0,62})/routes/([a-z][a-z0-9-]{0,62})/knowledge-health)", [&](const auto& request, auto& response) {
        query_fields(request, {});
        send(response, response_envelope(knowledge.health(principal(response), request.matches[1].str(), request.matches[2].str())));
    });
    server.Get(R"(/api/v1/projects/([a-z][a-z0-9-]{0,62})/knowledge-routes)", [&](const auto& request, auto& response) {
        query_fields(request, {});
        send(response, response_envelope(knowledge.routes(principal(response), request.matches[1].str())));
    });
    server.Post("/api/v1/session-bootstrap", [&](const auto& request, auto& response) {
        query_fields(request, {});
        send(response, response_envelope(knowledge.bootstrap(principal(response), request_json(request))));
    });
    server.Get("/api/v1/telemetry/retrieval/journal/epochs/reclamation", [&](const auto& request, auto& response) {
        query_fields(request, {"through"});
        send(response, response_envelope(store.plan_journal_reclamation(principal(response),
            query_number(request, "through", 0, 1, INT64_MAX))));
    });
    server.Post("/api/v1/telemetry/retrieval/journal/epochs/reclamation", [&](const auto& request, auto& response) {
        query_fields(request, {});
        send(response, response_envelope(store.apply_journal_reclamation(principal(response), request_json(request))));
    });
    server.Get("/api/v1/telemetry/retrieval/journal/producers", [&](const auto& request, auto& response) {
        query_fields(request, {"after", "limit"});
        send(response, response_envelope(store.retrieval_journal_producers(principal(response),
            request.get_param_value("after"), query_number(request, "limit", 20, 1, 32))));
    });
    server.Get(R"(/api/v1/telemetry/retrieval/journal/producers/([0-9a-f]{64})/retention)", [&](const auto& request, auto& response) {
        query_fields(request, {"through"});
        send(response, response_envelope(store.plan_journal_retention(principal(response), request.matches[1].str(),
            query_number(request, "through", 0, 1, INT64_MAX))));
    });
    server.Post(R"(/api/v1/telemetry/retrieval/journal/producers/([0-9a-f]{64})/retention)", [&](const auto& request, auto& response) {
        query_fields(request, {});
        send(response, response_envelope(store.apply_journal_retention(principal(response), request.matches[1].str(), request_json(request))));
    });
    for (const auto* report : {"summary", "queries"}) {
        const bool queries = std::string_view(report) == "queries";
        server.Get(std::string("/api/v1/telemetry/retrieval/journal/") + report, [&, queries](const auto& request, auto& response) {
            query_fields(request, {"project_id"});
            send(response, response_envelope(store.retrieval_journal_report(principal(response),
                request.get_param_value("project_id"), queries)));
        });
    }
    server.Get(R"(/api/v1/telemetry/retrieval/journal/attempts/([1-9][0-9]{0,18}))", [&](const auto& request, auto& response) {
        query_fields(request, {});
        const auto text = request.matches[1].str();
        std::uint64_t id = 0;
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), id);
        if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || id > INT64_MAX)
            throw ServiceError(400, "invalid-query", "Journal ID is invalid.");
        send(response, response_envelope(store.retrieval_journal_attempt(principal(response), id)));
    });
    server.Get("/api/v1/recovery", [&](const auto& request, auto& response) {
        query_fields(request, {});
        send(response, response_envelope(store.recovery_status(principal(response))));
    });
    server.Get("/api/v1/projects", [&](const auto& request, auto& response) {
        query_fields(request, {"after", "limit"});
        const auto after = request.get_param_value("after");
        if (request.has_param("after") && !valid_identifier(after))
            throw ServiceError(400, "invalid-query", "Query parameters are invalid.");
        const auto limit = query_number(request, "limit", 50, 1, 100);
        send(response, response_envelope(store.list_projects(principal(response), after, limit)));
    });
    server.Get(R"(/api/v1/projects/([a-z][a-z0-9-]{0,62})/task-checkpoints)", [&](const auto& request, auto& response) {
        query_fields(request, {"after", "limit"});
        send(response, response_envelope(store.list_checkpoints(principal(response), request.matches[1],
            request.get_param_value("after"), query_number(request, "limit", 16, 1, 32))));
    });
    server.Post(R"(/api/v1/projects/([a-z][a-z0-9-]{0,62})/task-checkpoints/prune)", [&](const auto& request, auto& response) {
        query_fields(request, {});
        const auto input = request_json(request);
        require_fields(input, {"schema_version"});
        if (input.at("schema_version") != "shin.checkpoint-prune-request.v1")
            throw ServiceError(400, "invalid-request", "Checkpoint prune request schema is invalid.");
        send(response, response_envelope(store.prune_checkpoints(principal(response), request.matches[1])));
    });
    server.Get(R"(/api/v1/projects/([a-z][a-z0-9-]{0,62})/task-checkpoints/([a-z][a-z0-9-]{0,62}))", [&](const auto& request, auto& response) {
        query_fields(request, {});
        const auto record = store.checkpoint_record(principal(response), request.matches[1], request.matches[2]);
        if (record.is_null()) throw ServiceError(404, "checkpoint-not-found", "Private checkpoint is unavailable.");
        send(response, response_envelope(checkpoint_public_record(record)));
    });
    server.Post(R"(/api/v1/projects/([a-z][a-z0-9-]{0,62})/task-checkpoints/([a-z][a-z0-9-]{0,62}))", [&](const auto& request, auto& response) {
        query_fields(request, {});
        send(response, response_envelope(continuity.save(principal(response), request.matches[1], request.matches[2], request_json(request))));
    });
    server.Post(R"(/api/v1/projects/([a-z][a-z0-9-]{0,62})/task-checkpoints/([a-z][a-z0-9-]{0,62})/(resume|impact))", [&](const auto& request, auto& response) {
        query_fields(request, {});
        send(response, response_envelope(continuity.resume(principal(response), request.matches[1], request.matches[2],
            request_json(request), request.matches[3] == "impact")));
    });
    server.Post(R"(/api/v1/projects/([a-z][a-z0-9-]{0,62})/task-checkpoints/([a-z][a-z0-9-]{0,62})/receipts)", [&](const auto& request, auto& response) {
        query_fields(request, {});
        send(response, response_envelope(continuity.import_receipts(principal(response), request.matches[1], request.matches[2], request_json(request))));
    });
    server.Post(R"(/api/v1/projects/([a-z][a-z0-9-]{0,62})/task-checkpoints/([a-z][a-z0-9-]{0,62})/clear)", [&](const auto& request, auto& response) {
        query_fields(request, {});
        send(response, response_envelope(continuity.clear(principal(response), request.matches[1], request.matches[2], request_json(request))));
    });
    server.Get(R"(/api/v1/projects/([a-z][a-z0-9-]{0,62}))", [&](const auto& request, auto& response) {
        query_fields(request, {});
        send(response, response_envelope(store.get_project(principal(response), request.matches[1].str())));
    });
    server.Post("/api/v1/projects", [&](const auto& request, auto& response) {
        const auto& actor = principal(response);
        if (!actor.admin) throw ServiceError(403, "forbidden", "This operation requires administrative permission.");
        query_fields(request, {});
        const auto key = idempotency_key(request);
        const auto input = project_input(config, request_json(request));
        const auto hash = digest_hex(sha256(Json{{"id", input.id}, {"display_name", input.display_name}}.dump()));
        const auto registered = store.register_project(actor, input, key, hash);
        send(response, response_envelope({{"project", registered.project}, {"replayed", registered.replayed}}),
             registered.replayed ? 200 : 201);
    });
    server.Get(R"(/api/v1/projects/([a-z][a-z0-9-]{0,62})/workspaces)", [&](const auto& request, auto& response) {
        query_fields(request, {"after", "limit"});
        const auto after = request.get_param_value("after");
        if (request.has_param("after") && !valid_identifier(after))
            throw ServiceError(400, "invalid-query", "Query parameters are invalid.");
        send(response, response_envelope(store.list_workspaces(principal(response), request.matches[1].str(),
             after, query_number(request, "limit", 50, 1, 100))));
    });
    server.Get(R"(/api/v1/projects/([a-z][a-z0-9-]{0,62})/workspaces/([a-z][a-z0-9-]{0,62}))", [&](const auto& request, auto& response) {
        query_fields(request, {});
        send(response, response_envelope(store.get_workspace(principal(response), request.matches[1].str(), request.matches[2].str())));
    });
    server.Post(R"(/api/v1/projects/([a-z][a-z0-9-]{0,62})/workspaces)", [&](const auto& request, auto& response) {
        const auto& actor = principal(response);
        if (!actor.admin) throw ServiceError(403, "forbidden", "This operation requires administrative permission.");
        query_fields(request, {});
        const auto project = request.matches[1].str();
        store.get_project(actor, project);
        const auto key = idempotency_key(request);
        const auto body = request_json(request);
        require_fields(body, {"id", "display_name", "root"});
        if (!valid_identifier(require_text(body, "id", 63)) ||
            !std::filesystem::path(require_text(body, "root", 4096)).is_absolute())
            throw ServiceError(400, "invalid-request", "Invalid workspace request.");
        require_text(body, "display_name", 120);
        const auto hash = digest_hex(sha256(Json{{"project_id", project},
             {"source_host_id", config.database.source_host_id}, {"request", body}}.dump()));
        const auto replay = store.workspace_registration_replay(actor, project, key, hash);
        if (!replay.is_null()) {
            send(response, response_envelope(replay));
            return;
        }
        const auto input = workspace_input(config, body);
        const auto result = store.register_workspace(actor, project, input, key, hash);
        send(response, response_envelope(result), result.at("replayed").template get<bool>() ? 200 : 201);
    });
    server.Get("/api/v1/audit", [&](const auto& request, auto& response) {
        query_fields(request, {"after", "limit"});
        const auto after = query_number(request, "after", 0, 0, std::numeric_limits<std::int64_t>::max());
        const auto limit = query_number(request, "limit", 50, 1, 100);
        send(response, response_envelope(store.audit_events(principal(response), after, limit)));
    });
    server.Get("/api/v1/audit/history", [&](const auto& request, auto& response) {
        query_fields(request, {});
        send(response, response_envelope(store.audit_history(principal(response))));
    });
    server.Get("/api/v1/audit/export", [&](const auto& request, auto& response) {
        const auto& actor = principal(response);
        if (!actor.admin) throw ServiceError(403, "forbidden", "Administrator access is required.");
        query_fields(request, {"limit"});
        send(response, response_envelope(store.export_audit(actor, query_number(request, "limit", 256, 1, 1000))));
    });
    server.Post("/api/v1/audit/retention", [&](const auto& request, auto& response) {
        const auto& actor = principal(response);
        if (!actor.admin) throw ServiceError(403, "forbidden", "Administrator access is required.");
        query_fields(request, {});
        send(response, response_envelope(store.apply_audit_retention(actor, request_json(request))));
    });
    const std::string personal_workspace = R"(/api/v1/personal/tenants/([a-z][a-z0-9-]{0,62})/projects/([a-z][a-z0-9-]{0,62})/workspaces/([a-z][a-z0-9-]{0,62}))";
    const auto observer_scope = [](const httplib::Request& request) {
        return ObserverScope{request.matches[2].str(), {request.matches[1].str(), request.matches[3].str()}};
    };
    const auto preparation_id = [](const std::string& input) {
        std::uint64_t result = 0;
        const auto parsed = std::from_chars(input.data(), input.data() + input.size(), result);
        if (parsed.ec != std::errc{} || parsed.ptr != input.data() + input.size() || result == 0 || result > INT64_MAX)
            throw ServiceError(400, "invalid-request", "Invalid preparation id.");
        return result;
    };
    server.Get(personal_workspace + "/task-checkpoints", [&](const auto& request, auto& response) {
        query_fields(request, {"after", "limit"});
        const auto scope = observer_scope(request);
        send(response, response_envelope(store.scoped_checkpoints(principal(response), scope.project_id,
            request.get_param_value("after"), query_number(request, "limit", 16, 1, 32), scope.workspace)));
    });
    server.Post(personal_workspace + "/task-checkpoints/prune", [&](const auto& request, auto& response) {
        query_fields(request, {});
        const auto input = request_json(request);
        require_fields(input, {"schema_version"});
        if (input.at("schema_version") != "shin.checkpoint-prune-request.v1")
            throw ServiceError(400, "invalid-request", "Checkpoint prune request schema is invalid.");
        const auto scope = observer_scope(request);
        send(response, response_envelope(store.prune_checkpoints(principal(response), scope.project_id, scope.workspace)));
    });
    const auto personal_checkpoint = personal_workspace + R"(/task-checkpoints/([a-z][a-z0-9-]{0,62}))";
    server.Get(personal_checkpoint, [&](const auto& request, auto& response) {
        query_fields(request, {});
        const auto scope = observer_scope(request);
        const auto record = store.scoped_checkpoint_record(principal(response), scope.project_id, request.matches[4], scope.workspace);
        if (record.is_null()) throw ServiceError(404, "checkpoint-not-found", "Private checkpoint is unavailable.");
        send(response, response_envelope(checkpoint_public_record(record)));
    });
    server.Post(personal_checkpoint, [&](const auto& request, auto& response) {
        query_fields(request, {});
        const auto scope = observer_scope(request);
        const auto input = request_json(request);
        if (!input.contains("selector") || !input.at("selector").is_object() || input.at("selector").value("kind", Json()) != "route")
            throw ServiceError(400, "invalid-request", "Personal checkpoints require a route selector.");
        send(response, response_envelope(continuity.save(principal(response), scope.project_id, request.matches[4], input, scope.workspace)));
    });
    server.Post(personal_checkpoint + R"(/(resume|impact|clear|receipts))", [&](const auto& request, auto& response) {
        query_fields(request, {});
        const auto scope = observer_scope(request);
        const auto& actor = principal(response);
        const auto input = request_json(request);
        const auto slot = request.matches[4].str(), action = request.matches[5].str();
        const auto result = action == "clear" ? continuity.clear(actor, scope.project_id, slot, input, scope.workspace) :
            action == "receipts" ? continuity.import_receipts(actor, scope.project_id, slot, input, scope.workspace) :
            continuity.resume_workspace(actor, scope.project_id, slot, scope.workspace, input, action == "impact", true);
        send(response, response_envelope(result));
    });
    server.Get(personal_workspace + "/preparations", [&](const auto& request, auto& response) {
        query_fields(request, {"after", "limit"});
        const auto scope = observer_scope(request);
        send(response, response_envelope(store.scoped_preparations(principal(response), scope.project_id,
            query_number(request, "after", 0, 0, INT64_MAX), query_number(request, "limit", 16, 1, 100), scope.workspace)));
    });
    const auto personal_job = personal_workspace + R"(/preparations/([1-9][0-9]{0,18}))";
    server.Get(personal_job, [&](const auto& request, auto& response) {
        query_fields(request, {});
        const auto scope = observer_scope(request);
        send(response, response_envelope(store.scoped_preparation(principal(response), scope.project_id,
            preparation_id(request.matches[4]), scope.workspace)));
    });
    server.Get(personal_job + R"(/(inspection|preflight|retention))", [&](const auto& request, auto& response) {
        query_fields(request, {});
        const auto scope = observer_scope(request);
        const auto& actor = principal(response);
        const auto write = store.capture_workspace_write(actor, scope.project_id, scope.workspace);
        const auto id = preparation_id(request.matches[4]);
        const auto action = request.matches[5].str();
        send(response, response_envelope(action == "inspection" ? preparations.inspect(actor, id, write) :
            action == "preflight" ? preparations.preflight(actor, id, write) : preparations.retention_plan(actor, id, write)));
    });
    server.Post(personal_job + R"(/(cancel|retention))", [&](const auto& request, auto& response) {
        query_fields(request, {});
        const auto scope = observer_scope(request);
        const auto& actor = principal(response);
        const auto input = request_json(request);
        const auto write = store.capture_workspace_write(actor, scope.project_id, scope.workspace);
        const auto id = preparation_id(request.matches[4]);
        if (request.matches[5] == "cancel") {
            require_fields(input, {});
            send(response, response_envelope(store.cancel_preparation(actor, id, write)));
        } else send(response, response_envelope(preparations.apply_retention(actor, id, input, write)));
    });
    server.Post(personal_workspace + R"(/routes/([a-z][a-z0-9-]{0,62})/preparations)", [&](const auto& request, auto& response) {
        query_fields(request, {});
        const auto scope = observer_scope(request);
        const auto result = preparations.enqueue(principal(response), scope.project_id, request.matches[4],
            request_json(request), idempotency_key(request), scope.workspace);
        send(response, response_envelope(result), result.at("replayed").template get<bool>() ? 200 : 202);
    });
    server.Get(personal_workspace + R"(/routes/([a-z][a-z0-9-]{0,62})/activation)", [&](const auto& request, auto& response) {
        query_fields(request, {});
        const auto scope = observer_scope(request);
        send(response, response_envelope(preparations.active_generation(principal(response), scope.project_id, request.matches[4], scope.workspace)));
    });
    server.Post(personal_workspace + R"(/routes/([a-z][a-z0-9-]{0,62})/activation)", [&](const auto& request, auto& response) {
        query_fields(request, {});
        const auto scope = observer_scope(request);
        send(response, response_envelope(preparations.activate(principal(response), scope.project_id,
            request.matches[4], request_json(request), scope.workspace)));
    });
    server.Post(personal_workspace + R"(/routes/([a-z][a-z0-9-]{0,62})/(search|brief|pack))", [&](const auto& request, auto& response) {
        query_fields(request, {});
        const auto scope = observer_scope(request);
        send(response, response_envelope(retrieval.retrieve_workspace(store, principal(response), scope.project_id,
            request.matches[4].str(), scope.workspace, request.matches[5].str(), request_json(request))));
    });
    server.Get(personal_workspace + "/journal", [&](const auto& request, auto& response) {
        query_fields(request, {"after", "limit"});
        const auto scope = observer_scope(request);
        send(response, response_envelope(store.scoped_retrieval_journal(principal(response), scope.project_id, scope.workspace,
            query_number(request, "after", 0, 0, INT64_MAX), query_number(request, "limit", 16, 1, 32))));
    });
    server.Get(personal_workspace + R"(/journal/([1-9][0-9]{0,18}))", [&](const auto& request, auto& response) {
        query_fields(request, {});
        const auto scope = observer_scope(request);
        const auto text = request.matches[4].str();
        std::uint64_t id = 0;
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), id);
        if (parsed.ec != std::errc{} || id > INT64_MAX) throw ServiceError(400, "invalid-query", "Invalid journal ID.");
        send(response, response_envelope(store.scoped_retrieval_journal_attempt(principal(response), scope.project_id, scope.workspace, id)));
    });
    server.Get("/api/v1/personal/observer-identity", [&](const auto& request, auto& response) {
        query_fields(request, {});
        const auto& actor = principal(response);
        store.identity_request_admission(actor, true);
        send(response, response_envelope(store.observer_identity(actor)));
    });
    server.Post(personal_workspace + "/observer-streams", [&](const auto& request, auto& response) {
        query_fields(request, {});
        const auto& actor = principal(response);
        const auto scope = observer_scope(request);
        auto body = request_json(request);
        require_fields(body, {"stream_id", "observer_id", "source_host_id", "profile", "workspace_binding_digest"});
        body["principal_id"] = actor.id;
        body["project_id"] = scope.project_id;
        body["workspace_id"] = scope.workspace.workspace_id;
        const auto result = store.enroll_observer(actor, body, scope);
        send(response, response_envelope(result), result.at("replayed").template get<bool>() ? 200 : 201);
    });
    const auto personal_stream = personal_workspace + R"(/observer-streams/([0-9a-f]{32}))";
    server.Get(personal_stream, [&](const auto& request, auto& response) {
        query_fields(request, {});
        send(response, response_envelope(store.get_observer(principal(response), request.matches[4].str(), observer_scope(request))));
    });
    server.Get(personal_stream + R"(/(retention|reclamation|discard))", [&](const auto& request, auto& response) {
        query_fields(request, {});
        const auto& actor = principal(response);
        const auto scope = observer_scope(request);
        const auto stream = request.matches[4].str(), action = request.matches[5].str();
        send(response, response_envelope(action == "retention" ? store.observer_retention(actor, stream, scope) :
            store.observer_reclamation(actor, stream, action == "discard" ? ObserverReclamationMode::discard_unconfirmed : ObserverReclamationMode::confirmed, scope)));
    });
    server.Post(personal_stream + R"(/(submissions|confirmations|reconciliation|revoke|retention|reclamation|discard))", [&](const auto& request, auto& response) {
        query_fields(request, {});
        const auto& actor = principal(response);
        const auto scope = observer_scope(request);
        const auto stream = request.matches[4].str(), action = request.matches[5].str();
        const auto input = request_json(request);
        Json result;
        if (action == "submissions") result = store.ingest_observation(actor, stream, input, scope);
        else if (action == "confirmations") result = store.confirm_observer(actor, stream, input, scope);
        else if (action == "reconciliation") result = store.reconcile_observer(actor, stream, input, scope);
        else if (action == "revoke") { require_fields(input, {}); result = store.revoke_observer(actor, stream, scope); }
        else if (action == "retention") result = store.apply_observer_retention(actor, stream, input, scope);
        else result = store.apply_observer_reclamation(actor, stream, input,
            action == "discard" ? ObserverReclamationMode::discard_unconfirmed : ObserverReclamationMode::confirmed, scope);
        const int status = action == "submissions" && !result.at("replayed").template get<bool>() ? 201 : 200;
        send(response, response_envelope(result), status);
    });
    server.Get("/api/v1/observer-identity", [&](const auto& request, auto& response) {
        query_fields(request, {});
        send(response, response_envelope(store.observer_identity(principal(response))));
    });
    server.Post(R"(/api/v1/projects/([a-z][a-z0-9-]{0,62})/observer-workspaces)", [&](const auto& request, auto& response) {
        const auto& actor = principal(response);
        if (!actor.admin) throw ServiceError(403, "forbidden", "Administrator access is required.");
        query_fields(request, {});
        const auto body = request_json(request);
        require_fields(body, {"id", "display_name", "source_host_id"});
        WorkspaceInput input;
        input.id = require_text(body, "id", 63);
        input.display_name = require_text(body, "display_name", 120);
        input.source_host_id = require_text(body, "source_host_id", 63);
        input.observer_source = true;
        const auto project = request.matches[1].str();
        const auto hash = digest_hex(sha256(Json{{"project_id", project}, {"kind", "observer"}, {"request", body}}.dump()));
        const auto result = store.register_workspace(actor, project, input, idempotency_key(request), hash);
        send(response, response_envelope(result), result.at("replayed").template get<bool>() ? 200 : 201);
    });
    server.Post("/api/v1/observer-streams", [&](const auto& request, auto& response) {
        const auto& actor = principal(response);
        if (!actor.admin) throw ServiceError(403, "forbidden", "Administrator access is required.");
        query_fields(request, {});
        const auto body = request_json(request);
        validate_enrollment(body);
        const auto producer = std::find_if(config.credentials.begin(), config.credentials.end(), [&](const auto& credential) {
            return credential.principal.id == body.at("principal_id").template get<std::string>();
        });
        if (producer == config.credentials.end() || producer->principal.admin ||
            std::find(producer->principal.project_ids.begin(), producer->principal.project_ids.end(), body.at("project_id").template get<std::string>()) == producer->principal.project_ids.end())
            throw ServiceError(400, "invalid-producer", "Enroll a configured non-administrator with explicit project access.");
        const auto result = store.enroll_observer(actor, body);
        send(response, response_envelope(result), result.at("replayed").template get<bool>() ? 200 : 201);
    });
    server.Get(R"(/api/v1/observer-streams/([0-9a-f]{32}))", [&](const auto& request, auto& response) {
        query_fields(request, {});
        send(response, response_envelope(store.get_observer(principal(response), request.matches[1].str())));
    });
    server.Post(R"(/api/v1/observer-streams/([0-9a-f]{32})/revoke)", [&](const auto& request, auto& response) {
        const auto& actor = principal(response);
        if (!actor.admin) throw ServiceError(403, "forbidden", "Administrator access is required.");
        query_fields(request, {});
        require_fields(request_json(request), {});
        send(response, response_envelope(store.revoke_observer(actor, request.matches[1].str())));
    });
    server.Post(R"(/api/v1/observer-streams/([0-9a-f]{32})/submissions)", [&](const auto& request, auto& response) {
        query_fields(request, {});
        const auto result = store.ingest_observation(principal(response), request.matches[1].str(), request_json(request));
        send(response, response_envelope(result), result.at("replayed").template get<bool>() ? 200 : 201);
    });
    server.Post(R"(/api/v1/observer-streams/([0-9a-f]{32})/confirmations)", [&](const auto& request, auto& response) {
        query_fields(request, {});
        send(response, response_envelope(store.confirm_observer(principal(response), request.matches[1].str(), request_json(request))));
    });
    server.Get(R"(/api/v1/observer-streams/([0-9a-f]{32})/retention)", [&](const auto& request, auto& response) {
        query_fields(request, {});
        send(response, response_envelope(store.observer_retention(principal(response), request.matches[1].str())));
    });
    server.Post(R"(/api/v1/observer-streams/([0-9a-f]{32})/retention)", [&](const auto& request, auto& response) {
        query_fields(request, {});
        send(response, response_envelope(store.apply_observer_retention(principal(response), request.matches[1].str(), request_json(request))));
    });
    server.Post(R"(/api/v1/observer-streams/([0-9a-f]{32})/reconciliation)", [&](const auto& request, auto& response) {
        query_fields(request, {});
        send(response, response_envelope(store.reconcile_observer(principal(response), request.matches[1].str(), request_json(request))));
    });
    server.Get(R"(/api/v1/observer-streams/([0-9a-f]{32})/reclamation)", [&](const auto& request, auto& response) {
        query_fields(request, {});
        send(response, response_envelope(store.observer_reclamation(principal(response), request.matches[1].str())));
    });
    server.Post(R"(/api/v1/observer-streams/([0-9a-f]{32})/reclamation)", [&](const auto& request, auto& response) {
        query_fields(request, {});
        send(response, response_envelope(store.apply_observer_reclamation(principal(response), request.matches[1].str(), request_json(request))));
    });
    server.Get(R"(/api/v1/observer-streams/([0-9a-f]{32})/discard)", [&](const auto& request, auto& response) {
        query_fields(request, {});
        send(response, response_envelope(store.observer_reclamation(principal(response), request.matches[1].str(),
            ObserverReclamationMode::discard_unconfirmed)));
    });
    server.Post(R"(/api/v1/observer-streams/([0-9a-f]{32})/discard)", [&](const auto& request, auto& response) {
        query_fields(request, {});
        send(response, response_envelope(store.apply_observer_reclamation(principal(response), request.matches[1].str(),
            request_json(request), ObserverReclamationMode::discard_unconfirmed)));
    });
    server.Get("/api/v1/cache", [&](const auto& request, auto& response) {
        query_fields(request, {});
        send(response, response_envelope(retrieval.cache_statistics(principal(response))));
    });
    server.Get("/api/v1/preparations", [&](const auto& request, auto& response) {
        query_fields(request, {"after", "limit"});
        const auto after = query_number(request, "after", 0, 0, std::numeric_limits<std::int64_t>::max());
        const auto limit = query_number(request, "limit", 50, 1, 100);
        send(response, response_envelope(store.list_preparations(principal(response), after, limit)));
    });
    server.Get("/api/v1/preparation-worker", [&](const auto& request, auto& response) {
        query_fields(request, {});
        if (!principal(response).admin)
            throw ServiceError(403, "forbidden", "Administrator access is required.");
        send(response, response_envelope(preparations.status()));
    });
    server.Get(R"(/api/v1/preparations/([1-9][0-9]{0,18}))", [&](const auto& request, auto& response) {
        query_fields(request, {});
        send(response, response_envelope(store.get_preparation(principal(response), preparation_id(request.matches[1].str()))));
    });
    server.Post(R"(/api/v1/preparations/([1-9][0-9]{0,18})/cancel)", [&](const auto& request, auto& response) {
        query_fields(request, {});
        require_fields(request_json(request), {});
        send(response, response_envelope(store.cancel_preparation(principal(response), preparation_id(request.matches[1].str()))));
    });
    server.Get(R"(/api/v1/preparations/([1-9][0-9]{0,18})/inspection)", [&](const auto& request, auto& response) {
        query_fields(request, {});
        send(response, response_envelope(preparations.inspect(principal(response), preparation_id(request.matches[1].str()))));
    });
    server.Get(R"(/api/v1/projects/([a-z][a-z0-9-]{0,62})/routes/([a-z][a-z0-9-]{0,62})/activation)", [&](const auto& request, auto& response) {
        query_fields(request, {});
        send(response, response_envelope(preparations.active_generation(principal(response), request.matches[1].str(), request.matches[2].str())));
    });
    server.Post(R"(/api/v1/projects/([a-z][a-z0-9-]{0,62})/routes/([a-z][a-z0-9-]{0,62})/activation)", [&](const auto& request, auto& response) {
        query_fields(request, {});
        send(response, response_envelope(preparations.activate(principal(response), request.matches[1].str(), request.matches[2].str(), request_json(request))));
    });
    server.Get(R"(/api/v1/preparations/([1-9][0-9]{0,18})/preflight)", [&](const auto& request, auto& response) {
        query_fields(request, {});
        send(response, response_envelope(preparations.preflight(principal(response), preparation_id(request.matches[1].str()))));
    });
    server.Get(R"(/api/v1/preparations/([1-9][0-9]{0,18})/retention)", [&](const auto& request, auto& response) {
        query_fields(request, {});
        send(response, response_envelope(preparations.retention_plan(principal(response), preparation_id(request.matches[1].str()))));
    });
    server.Post(R"(/api/v1/preparations/([1-9][0-9]{0,18})/retention)", [&](const auto& request, auto& response) {
        query_fields(request, {});
        send(response, response_envelope(preparations.apply_retention(principal(response),
            preparation_id(request.matches[1].str()), request_json(request))));
    });
    server.Post(R"(/api/v1/projects/([a-z][a-z0-9-]{0,62})/routes/([a-z][a-z0-9-]{0,62})/preparations)",
                [&](const auto& request, auto& response) {
        query_fields(request, {});
        const auto result = preparations.enqueue(principal(response), request.matches[1].str(),
            request.matches[2].str(), request_json(request), idempotency_key(request));
        send(response, response_envelope(result), result.at("replayed").template get<bool>() ? 200 : 202);
    });
    server.Post(R"(/api/v1/projects/([a-z][a-z0-9-]{0,62})/routes/([a-z][a-z0-9-]{0,62})/(search|brief|pack))",
                [&](const auto& request, auto& response) {
        query_fields(request, {});
        send(response, response_envelope(retrieval.retrieve(store, principal(response),
            request.matches[1].str(), request.matches[2].str(), request.matches[3].str(), request_json(request))));
    });
    server.Get("/api/v1/publication-history", [&](const auto& request, auto& response) {
        query_fields(request, {});
        send(response, response_envelope(store.publication_history(principal(response))));
    });
    server.Get("/api/v1/publication-history/retention", [&](const auto& request, auto& response) {
        query_fields(request, {"limit"});
        send(response, response_envelope(store.plan_publication_retention(principal(response),
            query_number(request, "limit", 64, 1, 256))));
    });
    server.Post("/api/v1/publication-history/retention", [&](const auto& request, auto& response) {
        query_fields(request, {});
        send(response, response_envelope(publications.retire_history(principal(response), request_json(request))));
    });
    server.Get(R"(/api/v1/projects/([a-z][a-z0-9-]{0,62})/publications/([a-z][a-z0-9-]{0,62}))", [&](const auto& request, auto& response) {
        query_fields(request, {});
        send(response, response_envelope(publications.head(principal(response), request.matches[1].str(), request.matches[2].str())));
    });
    server.Post(R"(/api/v1/projects/([a-z][a-z0-9-]{0,62})/publications/([a-z][a-z0-9-]{0,62})/proposals)", [&](const auto& request, auto& response) {
        query_fields(request, {});
        const auto result = publications.submit(principal(response), request.matches[1].str(), request.matches[2].str(),
                                                request_json(request), idempotency_key(request));
        send(response, response_envelope(result), result.at("replayed").template get<bool>() ? 200 : 201);
    });
    server.Post(R"(/api/v1/projects/([a-z][a-z0-9-]{0,62})/publications/([a-z][a-z0-9-]{0,62})/withdrawal)", [&](const auto& request, auto& response) {
        query_fields(request, {});
        send(response, response_envelope(publications.withdraw(principal(response), request.matches[1].str(),
            request.matches[2].str(), request_json(request))));
    });
    server.Get(R"(/api/v1/projects/([a-z][a-z0-9-]{0,62})/publications/([a-z][a-z0-9-]{0,62})/proposals)", [&](const auto& request, auto& response) {
        query_fields(request, {"after", "limit"});
        send(response, response_envelope(publications.list(principal(response), request.matches[1].str(), request.matches[2].str(),
            query_number(request, "after", 0, 0, std::numeric_limits<std::int64_t>::max()), query_number(request, "limit", 20, 1, 64))));
    });
    server.Get(R"(/api/v1/projects/([a-z][a-z0-9-]{0,62})/publications/([a-z][a-z0-9-]{0,62})/proposals/([1-9][0-9]{0,18}))", [&](const auto& request, auto& response) {
        query_fields(request, {});
        send(response, response_envelope(publications.detail(principal(response), request.matches[1].str(), request.matches[2].str(),
                                                             preparation_id(request.matches[3].str()))));
    });
    server.Get(R"(/api/v1/projects/([a-z][a-z0-9-]{0,62})/publications/([a-z][a-z0-9-]{0,62})/proposals/([1-9][0-9]{0,18})/review)", [&](const auto& request, auto& response) {
        query_fields(request, {});
        send(response, response_envelope(publications.review(principal(response), request.matches[1].str(), request.matches[2].str(),
                                                             preparation_id(request.matches[3].str()))));
    });
    server.Post(R"(/api/v1/projects/([a-z][a-z0-9-]{0,62})/publications/([a-z][a-z0-9-]{0,62})/proposals/([1-9][0-9]{0,18})/resolution)", [&](const auto& request, auto& response) {
        query_fields(request, {});
        send(response, response_envelope(publications.resolve(principal(response), request.matches[1].str(), request.matches[2].str(),
                                                              preparation_id(request.matches[3].str()), request_json(request))));
    });
    server.Post(R"(/api/v1/projects/([a-z][a-z0-9-]{0,62})/publications/([a-z][a-z0-9-]{0,62})/(search|brief|pack))", [&](const auto& request, auto& response) {
        query_fields(request, {});
        send(response, response_envelope(retrieval.retrieve_publication(store, principal(response), request.matches[1].str(),
            request.matches[2].str(), request.matches[3].str(), request_json(request))));
    });
    if (!server.bind_to_port(config.host, config.port))
        throw ServiceError(503, "listener-unavailable", "The configured listener is unavailable.");
    preparations.start();
    knowledge.start();
    journal_writer.start();
    std::thread stop_thread([&] {
        int signal = 0;
        if (sigwait(&signals, &signal) == 0) {
            server.stop();
            // Stop background polling while HTTP drains, not serially after the
            // journal flush. Destructors still join before dependencies die.
            knowledge.request_stop();
            preparations.request_stop();
        }
    });
    bool success = false;
    try {
        std::cerr << Json{{"event", "server-listening"}, {"version", kServerVersion},
                          {"host", config.host}, {"port", config.port}}.dump() << '\n';
        success = server.listen_after_bind();
    } catch (...) {
        pthread_kill(stop_thread.native_handle(), SIGUSR1);
        stop_thread.join();
        throw;
    }
    pthread_kill(stop_thread.native_handle(), SIGUSR1);
    stop_thread.join();
    journal_writer.shutdown();
    if (config.journal_writer.enabled)
        std::cerr << Json{{"event", "journal-writer-stopped"}, {"status", journal_writer.status()}}.dump() << '\n';
    return success ? 0 : 1;
}

} // namespace shin::server
