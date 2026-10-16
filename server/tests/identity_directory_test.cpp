#include "shin/server/identity_directory.h"
#include <array>
#include <iostream>

int main() {
    using namespace shin::server;
    struct Case { const char* role; bool read; bool manage; };
    const std::array cases{Case{"owner", true, false}, Case{"maintainer", false, false},
        Case{"reader", false, false}, Case{"admin", false, false}, Case{"", false, false},
        Case{"OWNER", false, false}, Case{"unknown", false, false}};
    for (const auto& c : cases) {
        if (identity_role_allows(c.role, IdentityOperation::tenant_read) != c.read ||
            identity_role_allows(c.role, IdentityOperation::membership_write) != c.manage) return 1;
    }
    std::cout << "personal identity role matrix: 14 decisions passed\n";
}
