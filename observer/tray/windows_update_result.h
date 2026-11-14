#pragma once
#include "shin/observer/io.h"
#include <string_view>

namespace shin::observer::windows_update {
enum class MsiOutcome { unconfirmed, installed, reboot_required };

inline MsiOutcome msi_outcome(const Json& code) {
    if (!code.is_number_integer()) return MsiOutcome::unconfirmed;
    if (code == 0) return MsiOutcome::installed;
    // ERROR_SUCCESS_REBOOT_INITIATED and ERROR_SUCCESS_REBOOT_REQUIRED.
    if (code == 1641 || code == 3010) return MsiOutcome::reboot_required;
    return MsiOutcome::unconfirmed;
}

inline bool update_can_retry_without_install(const Json& saved) {
    if (!saved.is_object()) return false;
    const auto phase = saved.value("phase", Json());
    if (phase == "reviewed" || phase == "checking" || phase == "downloading" ||
        phase == "verifying" || phase == "waiting-monitor") return true;
    if (phase != "review-required") return false;
    const auto error = saved.value("error_code", Json());
    if (error == "update-uac-cancelled" || error == "update-offer-changed" ||
        error == "update-download-invalid" || error == "update-helper-launch-failed" ||
        error == "update-monitor-running") return true;
    const auto code = saved.value("installer_exit_code", Json());
    // ERROR_INSTALL_PACKAGE_OPEN_FAILED occurs before any installation.
    return error == "update-msi-failed" && code.is_number_integer() && code == 1619;
}

// The caller confirms the machine-owned payload before recording either result.
inline void acknowledge_previous_installation(Json& saved, const std::string& version) {
    if (saved.value("from_version", Json()) != version || !update_can_retry_without_install(saved))
        throw Error("update-outcome-unknown", "Review Windows Installer before retrying. Its outcome could not be confirmed.");
    saved["installed_version"] = version;
    saved["phase"] = "acknowledged";
}

inline void record_installed_update(Json& saved, const std::string& version, Json preparation) {
    saved["projects_pending"] = !preparation.value("ok", false);
    saved["project_preparation"] = std::move(preparation);
    saved["installed_version"] = version;
    saved["phase"] = "completed";
    saved.erase("error_code");
}

struct UpdateResult {
    Json receipt = Json::object();
    bool active = false;
    bool needs_review = false;

    bool can_install() const { return !active && !needs_review; }
};

inline bool can_install_update(const UpdateResult& update, const Json& preparation, bool operation_pending) {
    return update.can_install() && !operation_pending && preparation.value("phase", Json()) != "preparing";
}

inline UpdateResult observe_update(Json saved, std::string_view current_version, bool writer_present) {
    if (!saved.is_object() || saved.value("schema_version", Json()) != "shin.windows-update.v1")
        throw Error("update-state-invalid", "Unknown update state.");
    const auto phase = saved.value("phase", Json());
    if (phase == "completed" || phase == "acknowledged") {
        // Acknowledgement verifies the original kit. Earlier writers store its
        // identity in from_version even when installed_version is absent.
        const auto version = saved.value("installed_version",
            phase == "acknowledged" ? saved.value("from_version", Json()) : Json());
        const auto offer = saved.value("offer", Json());
        const auto expected = phase == "acknowledged" ? saved.value("from_version", Json()) :
            offer.is_object() ? offer.value("version", Json()) : Json();
        if (version.is_string() && !version.get_ref<const std::string&>().empty() && version == expected) {
            if (version.get_ref<const std::string&>() != current_version) return {Json::object(), writer_present, false};
            return {std::move(saved), writer_present, false};
        }
    }
    // A previously running phase without a writer is an interrupted attempt.
    return {std::move(saved), writer_present, !writer_present};
}
}
