#include "../src/mcp_followups.h"
#include <iostream>
#include <stdexcept>

using namespace shin::observer;
int main() {
    try {
        const auto require = [](bool value) { if (!value) throw std::runtime_error("follow-up binding assertion"); };
        const auto refused = [&](McpFollowups& cache, const std::string& key) {
            try { (void)cache.resolve(key); } catch (const Error& error) {
                require(error.code == "mcp-query-binding-required"); return;
            }
            throw std::runtime_error("ambiguous, missing or expired binding was reused");
        };
        McpFollowups cache;
        refused(cache, "");
        auto first = cache.begin("one:chat");
        refused(cache, "one:chat");
        first->complete({"snapshot-a", "parent-a"});
        require(cache.resolve("one:chat").snapshot == "snapshot-a");
        require(cache.resolve("one:chat").parent == "parent-a");
        refused(cache, "one:other-chat"); refused(cache, "two:chat");
        auto left = cache.begin("one:chat");
        auto right = cache.begin("one:chat");
        left->complete({"snapshot-b", "parent-b"});
        right->complete({"snapshot-c", "parent-c"});
        refused(cache, "one:chat");
        cache.begin("one:chat")->complete({"snapshot-d", "parent-d"});
        require(cache.resolve("one:chat").snapshot == "snapshot-d");
        { auto failed = cache.begin("one:chat"); }
        refused(cache, "one:chat");
        McpFollowups expired(std::chrono::seconds{0});
        expired.begin("one:chat")->complete({"snapshot", "parent"});
        refused(expired, "one:chat");
        McpFollowups bounded;
        for (int i = 0; i < 129; ++i)
            bounded.begin(std::to_string(1000 + i))->complete({"snapshot", "parent"});
        refused(bounded, "1000");
        require(bounded.resolve("1128").snapshot == "snapshot");
        std::cout << "Follow-up scope, concurrency, expiry and capacity passed.\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
