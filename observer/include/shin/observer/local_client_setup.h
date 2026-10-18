#pragma once
#include "io.h"

namespace shin::observer {
// Read-only export. Does not enroll a root or edit a client.
Json local_client_setup(const fs::path& executable, const fs::path& root,
                        const fs::path& home, const std::string& client);
Json local_client_targets(const fs::path& user_home, char** environment);
Json local_client_connect(const fs::path& executable, const fs::path& root,
                          const fs::path& home, const std::string& client,
                          const fs::path& config, const std::string& revision);
}
