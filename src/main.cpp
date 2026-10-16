#include "context_internal.h"

int main(int argc, char** argv) {
    std::string command;
    try {
        command = shin::context::detect_command_name(argc, argv);
        return shin::context::run(argc, argv);
    } catch (const shin::context::CliError& ex) {
        shin::context::emit_error_result_noexcept(command, ex.code(), ex.category(), ex.what(), ex.exit_code(),
                                                  ex.retryable(), ex.remediation(), ex.diagnostic());
        return ex.exit_code();
    } catch (const std::bad_alloc&) {
        shin::context::emit_error_result_noexcept(
            command, "resource-exhausted", "resource", "The command could not allocate required memory.", 1,
            true, "Reduce the indexed corpus or configured limits, then retry.");
        return 1;
    } catch (const std::filesystem::filesystem_error& ex) {
        shin::context::emit_error_result_noexcept(
            command, "filesystem-error", "filesystem", ex.what(), 1, true,
            "Verify paths, permissions, and available storage before retrying.");
        return 1;
    } catch (const std::exception& ex) {
        shin::context::emit_error_result_noexcept(
            command, "execution-failed", "runtime", ex.what(), 1, false,
            "Inspect the profile and filesystem state; run build, check, or doctor as appropriate.");
        return 1;
    } catch (...) {
        shin::context::emit_error_result_noexcept(
            command, "unknown-failure", "internal",
            "The command failed with an unknown non-standard exception.", 1, false,
            "Report the command and arguments as a Shin Context Engine defect.");
        return 1;
    }
}
