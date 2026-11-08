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
    Json old{{"phase","installed"},{"ok",true},{"from_version","2.0.5-dev.98"},
        {"version","2.0.5-dev.100"},{"target_manifest_sha256","earlier-manifest"}};
    auto newer=installed;newer["version"]="2.0.5-dev.104";
    check(update_disposition(old,newer,"new-receipt")=="superseded");
    check(update_disposition(old,newer,"new-receipt",true)=="review-required");
    auto bad=newer;bad["integrity_verified"]=false;
    check(update_disposition(old,bad,"receipt")=="review-required");
    bad=newer;bad["recovery_pending"]=true;
    check(update_disposition(old,bad,"receipt")=="recovery-required");
    for(const auto* phase:{"downloading","installing","review-required"}) {
        auto active=old;active["phase"]=phase;
        check(update_disposition(active,newer,"receipt")=="review-required");
    }
    for(const auto* ver:{"2.0.5-dev.99","invalid"}) {
        auto other=newer;other["version"]=ver;
        check(update_disposition(old,other,"receipt")=="review-required");
    }
    auto same=newer;same["version"]="2.0.5-dev.100";
    check(update_disposition(old,same,"receipt")=="installed");
    Json result{{"ok",true},{"phase","installed"},{"outcome","superseded"}};
    check(update_result_allows_offer(result) && !update_result_installed(result));
    result["outcome"]="review-required";
    check(!update_result_allows_offer(result) && !update_result_installed(result));
    result["outcome"]="installed";check(update_result_installed(result));
    result["native_recovery_pending"]=true;check(!update_result_allows_offer(result));
    result["native_recovery_pending"]=false;result["updater_active"]=true;check(!update_result_allows_offer(result));
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
