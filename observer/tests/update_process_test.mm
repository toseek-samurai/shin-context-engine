// Exercise the actual helper's child transport without a network or installation.
#include "../src/local_updates_macos.mm"
#include <stdexcept>

int main(int argc, char** argv) {
 @autoreleasepool {
  try {
    if (argc != 2) return 2;
    using namespace shin::observer;
    const auto check = [](bool value) { if (!value) throw std::runtime_error("update child transport failed"); };
    char pattern[]="/tmp/shin-progress-test.XXXXXXXX";
    const auto temporary=::mkdtemp(pattern);check(temporary);
    const fs::path work=fs::canonical(temporary), acknowledged=work/"observed";
    struct Cleanup { fs::path path; ~Cleanup() { fs::remove_all(path); } } cleanup{work};
    // Display metadata must parse before read-only installation admission, and
    // cannot bypass that admission or reach the network for an unowned prefix.
    const auto arguments=[&](const std::string& version) {
        std::vector<std::string> words{"shin-ce","update-install","--prefix",(work/"missing").string(),
            "--wait-pid",std::to_string(getppid()),"--ui-language","ru","--ui-version",version,"--revision","fixture"};
        std::vector<char*> pointers;for(auto& word:words)pointers.push_back(word.data());
        try { (void)run_updater_request(static_cast<int>(pointers.size()),pointers.data(),work); }
        catch(const Error& error) { return error.code; }
        return std::string("unexpected-success");
    };
    check(arguments("2.0.5-preview")=="installation-not-owned");
    check(arguments("bad\nversion")=="invalid-arguments");
    const Json installed{{"integrity_verified",true},{"recovery_pending",false},{"version","2.0.5-dev.85"},{"manifest_sha256","old-manifest"}};
    check(update_disposition({{"phase","idle"}},installed,"receipt")=="idle");
    check(update_disposition({{"phase","reviewed"},{"acknowledged_version","2.0.5-dev.85"}},installed,"receipt")=="reviewed");
    check(update_disposition({{"phase","installed"},{"ok",true},{"version","2.0.5-dev.85"}},installed,"receipt")=="installed");
    Json attempt{{"phase","review-required"},{"from_version","2.0.5-dev.85"},{"version","2.0.5-dev.86"},
        {"result",{{"ok",false},{"error",{{"code","installation-project-not-ready"}}}}}};
    check(update_disposition(attempt,installed,"receipt")=="retryable");
    auto pending=installed;pending["recovery_pending"]=true;
    check(update_disposition(attempt,pending,"receipt")=="recovery-required");
    auto changed=installed;changed["version"]="2.0.5-dev.84";
    check(update_disposition(attempt,changed,"receipt")=="review-required");
    attempt["result"]["error"]["code"]="installation-ownership-changed";
    check(update_disposition(attempt,installed,"receipt")=="review-required");
    attempt.erase("result");attempt["error"]={{"code","update-outcome-unknown"}};
    check(update_disposition(attempt,installed,"receipt")=="review-required");
    attempt["before_receipt_sha256"]="receipt";
    check(update_disposition(attempt,installed,"receipt")=="retryable");
    check(update_disposition(attempt,installed,"different-receipt")=="review-required");
    changed["version"]="2.0.5-dev.86";
    check(update_disposition(attempt,changed,"receipt")=="review-required");
    attempt["target_manifest_sha256"]="target";changed["manifest_sha256"]="target";
    check(update_disposition(attempt,changed,"new-receipt")=="installed");
    // A substituted temporary directory cannot be reused or cleaned.
    Json retained{{"staging",work.string()},{"stage_identity","wrong"}};
    bool unsafe=false;
    try {(void)retained_stage(retained);}catch(const Error& e){unsafe=e.code=="update-staging-changed";}
    check(unsafe && fs::exists(work));
    std::vector<ReleaseUpdateStep> observed;
    const auto success = run(argv[1], {"-c",
        "import sys,time,json,pathlib\n"
        "p=pathlib.Path(sys.argv[1])\n"
        "print('{\"step\":13,\"received\":0,\"total\":0}',file=sys.stderr,flush=True)\n"
        "until=time.monotonic()+8\n"
        "while not p.exists() and time.monotonic()<until: time.sleep(.01)\n"
        "print('{\"step\":17,\"received\":0,\"total\":0}',file=sys.stderr)\n"
        "print(json.dumps({'ok':p.exists()}))", acknowledged.string()},
        10, nullptr, [&](auto value) { observed.push_back(value.step);(void)write_private_archive(acknowledged,"observed"); });
    check(success.at("ok") == true && observed == std::vector{ReleaseUpdateStep::starting_service});
    // The receipt waits for a live callback; child completion cannot certify the outer update.
    const auto refused = run(argv[1], {"-c",
        "import sys; print('{\"step\":16,\"received\":0,\"total\":0}',file=sys.stderr); "
        "print('{\"ok\":false,\"error\":{\"code\":\"fixture-refused\"}}',file=sys.stderr); sys.exit(2)"},
        5, nullptr, [](auto) { throw std::runtime_error("viewer unavailable"); });
    check(refused.at("error").at("code") == "fixture-refused");
    bool unknown = false;
    try { (void)run(argv[1], {"-c", "import sys; print('{\"step\":17,\"received\":0,\"total\":0}',file=sys.stderr)"},
        5, nullptr, [](auto) {}); }
    catch (const Error& error) { unknown = error.code == "update-outcome-unknown"; }
    check(unknown); // Progress alone is never a receipt.
    const auto noisy = run(argv[1], {"-c", "import sys; sys.stderr.write('x'*300000); print('{\"ok\":true}')"},
        5, nullptr, [](auto) {});
    check(noisy.at("ok") == true); // An unavailable/noisy observer cannot kill installation.
    std::cout << "Child progress, native refusal and independent receipt boundary passed\n";
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
 }
}
