#include "shin/server/config.h"
#include "shin/server/store.h"
#include "shin/server/retrieval_telemetry.h"
#include "shin/server/version.h"

#include <iostream>
#include <array>
#include <barrier>
#include <future>
#include <stdexcept>

namespace {
void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
}

int main(int argc, char** argv) {
    using namespace shin::server;
    try {
        if (argc != 3 && argc != 4) throw std::runtime_error("usage: journal-store-probe CONFIG INSTANCE_ID [capture|epochs|fences]");
        const std::string instance = argv[2];
        check(instance.size() == 64 && instance.find_first_not_of("0123456789abcdef") == std::string::npos, "invalid probe identity");
        const std::string mode = argc == 4 ? argv[3] : "legacy";
        check(mode == "legacy" || mode == "capture" || mode == "epochs" || mode == "fences", "invalid probe mode");
        const auto config = load_config(argv[1]);
        Store store(config.database);
        ObservabilityConfig policy;
        policy.max_retained_requests = mode == "fences" ? 4 : 1;
        RetrievalTelemetry telemetry(policy);
        const Principal actor{"owner", true, {}};
        auto register_producer = [&](const std::string& id) {
            return store.register_journal_producer(actor, Json{{"instance_id", id}, {"source_host_id", "journal-late-probe"},
                {"server_version", kServerVersion}, {"core_version", "synthetic-probe"}}).at("producer_epoch").get<std::int64_t>();
        };
        std::int64_t epoch = 0;
        auto record = [&] {
            auto attempt = telemetry.begin(RetrievalOperation::search, RetrievalSelector::route);
            telemetry.finish(attempt, "visible", "journal-probe", Json::object(), nullptr, 400, "probe-error");
        };
        auto snapshot = [&] {
            auto batch = telemetry.journal_snapshot();
            batch["instance_id"] = instance;
            batch["source_host_id"] = "journal-late-probe";
            batch["server_version"] = kServerVersion;
            batch["core_version"] = "synthetic-probe";
            batch["producer_epoch"] = epoch;
            for (auto& item : batch.at("records")) item["context"] = nullptr;
            return batch;
        };
        if (mode == "epochs") {
            auto instance_for = [&](std::size_t number) {
                static constexpr std::array<char, 16> hex{'0','1','2','3','4','5','6','7','8','9','a','b','c','d','e','f'};
                auto value = instance;
                for (std::size_t index = 0; index < 4; ++index)
                    value[60 + index] = hex[(number >> ((3 - index) * 4)) & 0x0fU];
                return value;
            };
            std::int64_t last_epoch = 0;
            for (std::size_t number = 0; number < 128; ++number) {
                const auto admission = register_producer(instance_for(number));
                check(number == 0 || admission == last_epoch + 1,
                      "producer epoch allocation was not monotonic from a reclaimed lifecycle");
                last_epoch = admission;
            }
            bool exhausted = false;
            try { static_cast<void>(register_producer(instance_for(128))); }
            catch (const ServiceError& error) { exhausted = error.code == "journal-capacity"; }
            check(exhausted, "129th active producer admission was not refused before reclamation");
            epoch = last_epoch + 1;
            bool future_epoch = false;
            try { static_cast<void>(store.capture_retrieval_journal(actor, snapshot())); }
            catch (const ServiceError& error) { future_epoch = error.code == "journal-producer-conflict"; }
            check(future_epoch, "unallocated future producer epoch was accepted for capture");
            const auto first_epoch = last_epoch - 127;
            for (std::int64_t through = first_epoch + 31; through <= last_epoch; through += 32) {
                const auto plan = store.plan_journal_reclamation(actor, through);
                check(plan.at("producer_count") == 32 && plan.at("through") == through,
                      "reclamation plan did not retain the bounded epoch prefix");
                const auto applied = store.apply_journal_reclamation(actor,
                    Json{{"plan", plan}, {"discard_future_confirmed", true}});
                check(applied.at("replayed") == false && applied.at("receipt").at("reclaimed_through") == through,
                      "reclamation did not advance the expected global epoch fence");
            }
            epoch = first_epoch;
            bool reclaimed = false;
            auto stale_batch = snapshot();
            stale_batch["instance_id"] = instance_for(0);
            try { static_cast<void>(store.capture_retrieval_journal(actor, stale_batch)); }
            catch (const ServiceError& error) { reclaimed = error.code == "journal-producer-reclaimed"; }
            check(reclaimed, "reclaimed producer epoch was silently accepted for capture");
            const auto recovery_epoch = register_producer(instance_for(128));
            check(recovery_epoch == last_epoch + 1, "producer admission did not recover with a new monotonic epoch");
            const auto recovery_plan = store.plan_journal_reclamation(actor, recovery_epoch);
            const auto recovery = store.apply_journal_reclamation(actor,
                Json{{"plan", recovery_plan}, {"discard_future_confirmed", true}});
            check(recovery.at("replayed") == false && recovery.at("receipt").at("reclaimed_through") == recovery_epoch,
                  "recovery producer was not reclaimed for a clean probe exit");
            const auto race_instance = instance_for(129);
            const auto race_epoch = register_producer(race_instance);
            check(race_epoch == recovery_epoch + 1, "race producer did not receive the next allocated epoch");
            epoch = race_epoch;
            auto identified = telemetry.begin(RetrievalOperation::brief, RetrievalSelector::route);
            const Json result{{"route_id", "journal-probe"}, {"workspace_id", "probe-workspace"},
                {"source_host_id", "journal-late-probe"}, {"snapshot_id", "fnv1a64:0123456789abcdef"},
                {"preparation_id", 1}, {"revision", 1}, {"evidence", {{"retrieval_mode", "identifier-scoped"}}}};
            telemetry.finish(identified, "visible", "journal-probe", Json::object(), &result, 200, "");
            auto initial_race = snapshot();
            initial_race["instance_id"] = race_instance;
            check(store.capture_retrieval_journal(actor, initial_race).at("inserted_count") == 1,
                  "race producer did not capture its initial observation");
            const auto persisted = store.retrieval_journal(actor, "visible", 0, 32).at("items");
            check(persisted.size() == 1 && persisted[0].at("instance_id") == race_instance &&
                      persisted[0].at("producer_epoch") == race_epoch &&
                      persisted[0].at("retrieval_mode") == "identifier-scoped",
                  "telemetry-to-PostgreSQL capture lost identifier-scoped retrieval mode");
            const auto initial_retention = store.plan_journal_retention(actor, race_instance, 1);
            check(store.apply_journal_retention(actor, race_instance,
                Json{{"plan", initial_retention}, {"discard_confirmed", true}}).at("replayed") == false,
                "race producer initial observation was not retired");
            const auto race_plan = store.plan_journal_reclamation(actor, race_epoch);
            record();
            auto next_race = snapshot();
            next_race["instance_id"] = race_instance;
            std::barrier race_start(2);
            auto capture = std::async(std::launch::async, [&] {
                race_start.arrive_and_wait();
                return store.capture_retrieval_journal(actor, next_race);
            });
            race_start.arrive_and_wait();
            bool apply_won = false;
            bool apply_stale = false;
            try {
                const auto applied = store.apply_journal_reclamation(actor,
                    Json{{"plan", race_plan}, {"discard_future_confirmed", true}});
                apply_won = applied.at("replayed") == false && applied.at("receipt").at("reclaimed_through") == race_epoch;
            } catch (const ServiceError& error) {
                apply_stale = error.code == "journal-reclamation-stale";
            }
            bool capture_won = false;
            bool capture_reclaimed = false;
            try {
                const auto captured = capture.get();
                capture_won = captured.at("inserted_count") == 1 && captured.at("replayed") == false;
            } catch (const ServiceError& error) {
                capture_reclaimed = error.code == "journal-producer-reclaimed";
            }
            check((capture_won && apply_stale && !apply_won && !capture_reclaimed) ||
                      (apply_won && capture_reclaimed && !capture_won && !apply_stale),
                  "capture versus reclamation did not resolve to an exact fenced outcome");
            if (capture_won) {
                const auto suffix_retention = store.plan_journal_retention(actor, race_instance, 2);
                check(store.apply_journal_retention(actor, race_instance,
                    Json{{"plan", suffix_retention}, {"discard_confirmed", true}}).at("replayed") == false,
                    "race capture suffix was not retired before cleanup");
                const auto suffix_reclamation = store.plan_journal_reclamation(actor, race_epoch);
                check(store.apply_journal_reclamation(actor,
                    Json{{"plan", suffix_reclamation}, {"discard_future_confirmed", true}}).at("replayed") == false,
                    "race capture suffix producer was not reclaimed after retirement");
            }
            std::cout << Json{{"ok", true}, {"instance_id", instance}, {"mode", "epochs"},
                              {"allocated_through", race_epoch}}.dump() << '\n';
            return 0;
        }
        epoch = register_producer(instance);
        if (mode == "fences") {
            record();
            record();
            const auto initial = snapshot();
            const auto captured = store.capture_retrieval_journal(actor, initial);
            check(captured.at("inserted_count") == 2 && captured.at("retired_ignored_count") == 0 &&
                      captured.at("retired_through") == 0, "initial capture retirement fields differ");
            const auto plan = store.plan_journal_retention(actor, instance, 1);
            record();
            const auto appended = snapshot();
            std::barrier start(2);
            auto capture = std::async(std::launch::async, [&] {
                start.arrive_and_wait();
                return store.capture_retrieval_journal(actor, appended);
            });
            start.arrive_and_wait();
            const auto applied = store.apply_journal_retention(actor, instance,
                Json{{"plan", plan}, {"discard_confirmed", true}});
            const auto append_capture = capture.get();
            check(append_capture.at("inserted_count") == 1 &&
                      append_capture.at("already_observed_count").get<int>() +
                      append_capture.at("retired_ignored_count").get<int>() == 2,
                  "concurrent append and retention lost or recreated observations");
            check(applied.at("replayed") == false && applied.at("receipt").at("retired_through") == 1,
                  "retention apply did not advance the expected fence");
            const auto replay = store.apply_journal_retention(actor, instance,
                Json{{"plan", plan}, {"discard_confirmed", true}});
            check(replay.at("replayed") == true && replay.at("receipt") == applied.at("receipt"),
                  "latest exact retention plan did not replay after append capture");
            auto wrong_server = plan;
            wrong_server["server_id"] = "wrong-server";
            bool stale = false;
            try { store.apply_journal_retention(actor, instance, Json{{"plan", wrong_server}, {"discard_confirmed", true}}); }
            catch (const ServiceError& error) { stale = error.code == "journal-retention-stale"; }
            check(stale, "old Server plan replayed before its identity was fenced");
            record();
            const auto mixed = snapshot();
            const auto mixed_capture = store.capture_retrieval_journal(actor, mixed);
            check(mixed_capture.at("inserted_count") == 1 && mixed_capture.at("already_observed_count") == 2 &&
                      mixed_capture.at("retired_ignored_count") == 1 && mixed_capture.at("retired_through") == 1,
                  "mixed retired and current capture did not keep the newer suffix");
            const auto after_tail = store.apply_journal_retention(actor, instance,
                Json{{"plan", plan}, {"discard_confirmed", true}});
            check(after_tail.at("replayed") == true && after_tail.at("receipt") == applied.at("receipt"),
                  "tail capture invalidated the latest exact retirement receipt");
            auto retired_only = initial;
            retired_only["records"].erase(1);
            retired_only["latest_sequence"] = 1;
            retired_only["records"][0]["reason_code"] = "changed-retired-content";
            const auto ignored = store.capture_retrieval_journal(actor, retired_only);
            check(ignored.at("inserted_count") == 0 && ignored.at("already_observed_count") == 0 &&
                      ignored.at("retired_ignored_count") == 1 && ignored.at("replayed") == true,
                  "retired content was resurrected or claimed to be a verified duplicate");
            const auto mixed_replay = store.capture_retrieval_journal(actor, mixed);
            check(mixed_replay.at("inserted_count") == 0 && mixed_replay.at("retired_ignored_count") == 1 &&
                      mixed_replay.at("replayed") == true, "retired-only replay changed durable capture state");
            auto later = retired_only;
            later["records"][0]["sequence"] = 6;
            later["latest_sequence"] = 6;
            const auto gap = store.capture_retrieval_journal(actor, later);
            check(gap.at("inserted_count") == 1 && gap.at("new_missing_sequences") == 1,
                  "capture above the fence did not retain its missing interval");
            const auto gap_plan = store.plan_journal_retention(actor, instance, 5);
            check(gap_plan.at("delete_count") == 3 && gap_plan.at("retire_missing_count") == 1,
                  "retirement prefix confused retained facts with missing sequences");
            store.apply_journal_retention(actor, instance, Json{{"plan", gap_plan}, {"discard_confirmed", true}});
            later["records"][0]["sequence"] = 5;
            later["latest_sequence"] = 5;
            const auto late_gap = store.capture_retrieval_journal(actor, later);
            check(late_gap.at("retired_ignored_count") == 1 && late_gap.at("recovered_missing_sequences") == 0,
                  "late delivery silently recovered a permanently retired gap");
            later["records"][0]["sequence"] = 7;
            later["latest_sequence"] = 7;
            check(store.capture_retrieval_journal(actor, later).at("inserted_count") == 1,
                  "retired missing prefix blocked newer facts");
            std::cout << Json{{"ok", true}, {"instance_id", instance}, {"mode", "fences"}}.dump() << '\n';
            return 0;
        }
        record();
        const auto earlier = snapshot();
        record();
        const auto later = snapshot();
        const auto first = store.capture_retrieval_journal(actor, later);
        check(first.at("inserted_count") == 1 && first.at("new_missing_sequences") == 1, "later snapshot prefix differs");
        const auto recovered = store.capture_retrieval_journal(actor, earlier);
        check(recovered.at("inserted_count") == 1, "late retained record was skipped");
        check(recovered.at("recovered_missing_sequences") == 1 && recovered.at("observed_through") == 2,
              "late capture failed to repair missing sequence accounting");
        const auto replay = store.capture_retrieval_journal(actor, earlier);
        check(replay.at("inserted_count") == 0 && replay.at("replayed") == true &&
                  replay.at("recovered_missing_sequences") == 0, "late retry duplicated facts");
        auto conflicting = earlier;
        conflicting["records"][0]["reason_code"] = "different-error";
        bool refused = false;
        try { store.capture_retrieval_journal(actor, conflicting); }
        catch (const ServiceError& error) { refused = error.code == "journal-fact-conflict"; }
        check(refused, "same sequence accepted changed content");
        auto fourth = earlier;
        fourth["records"][0]["sequence"] = 4;
        fourth["latest_sequence"] = 4;
        store.capture_retrieval_journal(actor, fourth);
        auto mixed = earlier;
        mixed["records"][0]["sequence"] = 3;
        auto sixth = earlier.at("records")[0];
        sixth["sequence"] = 6;
        mixed["records"].push_back(std::move(sixth));
        mixed["latest_sequence"] = 6;
        const auto repaired = store.capture_retrieval_journal(actor, mixed);
        check(repaired.at("inserted_count") == 2 && repaired.at("new_missing_sequences") == 1 &&
                  repaired.at("recovered_missing_sequences") == 1, "new and recovered gaps were incorrectly netted");
        if (mode == "capture") {
            check(first.at("retired_ignored_count") == 0 && first.at("retired_through") == 0 &&
                      replay.at("retired_ignored_count") == 0, "unretired capture reported a retirement fence");
            std::cout << Json{{"ok", true}, {"instance_id", instance}, {"mode", "capture"}}.dump() << '\n';
        } else {
            std::cout << Json{{"ok", true}, {"instance_id", instance}, {"synthetic", true}}.dump() << '\n';
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
