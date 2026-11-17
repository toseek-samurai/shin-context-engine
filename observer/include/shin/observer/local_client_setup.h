#pragma once
#include "io.h"

namespace shin::observer {
// Read-only export. Does not enroll a root or edit a client.
Json local_client_setup(const fs::path& executable, const fs::path& root,
                        const fs::path& home, const std::string& client);
Json local_client_targets(const fs::path& user_home, char** environment);
// Review/update the bundled Codex guidance. A reviewed revision is required for
// writes; customized files are preserved and interrupted writes are recoverable.
Json local_codex_skill(const fs::path& executable, const fs::path& config,
                       const std::string& revision = {});
// Re-read only this agent's explicit allowlist. Never enumerates registered roots.
std::vector<fs::path> local_client_live_roots(const fs::path& executable, const fs::path& home,
                                            const std::string& client, const fs::path& config);
Json local_client_connections(const fs::path& executable, const fs::path& root,
                              const fs::path& home, Json targets);
Json local_client_connect(const fs::path& executable, const fs::path& root,
                          const fs::path& home, const std::string& client,
                          const fs::path& config, const std::string& revision);
// Review each selected file, or apply the exact reviewed revisions in order.
// A failed row preserves earlier results and leaves later files untouched.
Json local_client_connect_batch(const fs::path& executable, const fs::path& root,
                                const fs::path& home, const Json& targets, bool apply);
}
