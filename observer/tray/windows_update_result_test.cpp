#include "windows_update_result.h"
#include <iostream>

using shin::observer::Error;
using shin::observer::Json;
using namespace shin::observer::windows_update;

int main(int argc, char** argv) {
    try {
        const auto require = [](bool ok, const char* message) {
            if (!ok) throw std::runtime_error(message);
        };
        const std::string previous = "2.0.5-dev.109", target = "2.0.5-dev.110";
        const Json attempt{{"schema_version", "shin.windows-update.v1"}, {"from_version", previous},
            {"offer", {{"version", target}}}, {"phase", "review-required"},
            {"error_code", "update-msi-failed"}, {"installer_exit_code", 1619}};
        require(argc == 2, "Pass the disposable receipt directory");
        const shin::observer::fs::path directory(argv[1]);
        shin::observer::ensure_private_directory(directory);
        const auto identity = shin::observer::directory_identity(directory, true);
        auto acknowledged = attempt;
        acknowledge_previous_installation(acknowledged, previous);
        {
            shin::observer::StateStore writer(directory, true, identity);
            writer.recover_pending();
            writer.save(acknowledged);
        }
        shin::observer::StateStore reader(directory, false, identity);
        auto current = observe_update(reader.load().value(), previous, reader.writer_present());
        require(current.can_install() && current.receipt.at("installed_version") == previous,
            "Acknowledgement did not remain usable after rereading the receipt");
        acknowledged.erase("installed_version");
        current = observe_update(acknowledged, previous, false);
        require(current.can_install() && current.receipt == acknowledged,
            "An existing acknowledgement of the current kit was discarded");
        require(observe_update(acknowledged, target, false).receipt.empty(),
            "An acknowledgement for a different kit remained current");

        auto interrupted = attempt;
        interrupted["phase"] = "installing";
        interrupted.erase("installer_exit_code");
        require(observe_update(interrupted, previous, true).active,
            "The live installer was treated as interrupted");
        current = observe_update(interrupted, previous, false);
        require(current.needs_review && !current.can_install(),
            "An interrupted MSI with no exit receipt was allowed to retry");
        try {
            acknowledge_previous_installation(interrupted, previous);
            throw std::runtime_error("Unknown MSI outcome was acknowledged");
        } catch (const Error& error) {
            require(error.code == "update-outcome-unknown", "Unknown MSI outcome lost its reason");
        }

        require(msi_outcome(0) == MsiOutcome::installed && msi_outcome(3010) == MsiOutcome::reboot_required &&
            msi_outcome(1641) == MsiOutcome::reboot_required, "MSI success and reboot codes were misclassified");
        for (const auto& code : Json::array({1603, 1619, 0.0, false, "0", nullptr}))
            require(msi_outcome(code) == MsiOutcome::unconfirmed, "Unconfirmed MSI exit was accepted");

        auto installed = attempt;
        installed["installer_exit_code"] = 0;
        record_installed_update(installed, target, {{"ok", false}});
        current = observe_update(installed, target, false);
        require(current.receipt.at("phase") == "completed" && current.receipt.at("projects_pending") == true &&
            !current.receipt.contains("error_code"), "Project preparation hid a verified installation");
        require(can_install_update(current, Json{{"phase", "review-required"}}, false),
            "Failed project preparation vetoed a later update");
        require(!can_install_update(current, Json{{"phase", "preparing"}}, false) &&
            !can_install_update(current, Json::object(), true) &&
            !can_install_update(observe_update(installed, target, true), Json::object(), false),
            "A new update could start while an installation or preparation writer was active");
        installed["installed_version"] = previous;
        require(observe_update(installed, previous, false).needs_review,
            "A completed attempt did not name the reviewed target");
        std::cout << "Windows update receipts: acknowledgement, interruption and project preparation passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
