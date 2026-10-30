#include "shin/observer/local_updates.h"
#include "shin/observer/updater_bundle.h"
#include <iostream>
using namespace shin::observer;
namespace {
void check(bool ok,const char* reason){if(!ok)throw std::runtime_error(reason);}
template<class Operation> void rejects(const char* code,Operation operation) {
    try { operation(); } catch(const Error& error) { check(error.code==code,"unexpected refusal");return; }
    throw std::runtime_error("unsafe JSON accepted");
}
Json release(const std::string& version,bool preview=true) {
    const auto tag="v"+version,name="shin-local-"+version+"-macos-arm64.zip";
    const std::string base="https://github.com/toseek-samurai/shin-context-engine/releases/";
    return {{"tag_name",tag},{"draft",false},{"prerelease",preview},{"html_url",base+"tag/"+tag},{"assets",Json::array({
        {{"name",name},{"state","uploaded"},{"size",1000},{"digest","sha256:"+std::string(64,'a')},{"browser_download_url",base+"download/"+tag+"/"+name}},
        {{"name",name+".sha256"},{"state","uploaded"},{"size",110},{"browser_download_url",base+"download/"+tag+"/"+name+".sha256"}}
    })}};
}
void refused(Json r){check(!select_local_release(Json::array({r}),"2.0.5-dev.16").at("available"),"unsafe release offered");}
Json windows_release(const std::string& version) {
    auto r=release(version);
    r["assets"]=Json::array();
    for (const auto* suffix:{".msi",".zip"}) {
        const auto name="shin-desktop-"+version+"-windows-AMD64"+suffix;
        const auto base="https://github.com/toseek-samurai/shin-context-engine/releases/download/v"+version+"/";
        r["assets"].push_back({{"name",name},{"state","uploaded"},{"size",1000},{"digest","sha256:"+std::string(64,'a')},{"browser_download_url",base+name}});
        r["assets"].push_back({{"name",name+".sha256"},{"state","uploaded"},{"size",110},{"browser_download_url",base+name+".sha256"}});
    }
    return r;
}
void windows_release_selection() {
    const auto a=windows_release("2.0.5-dev.17"),b=windows_release("2.0.5-dev.100");
    const auto selected=select_windows_release(Json::array({b,release("2.0.5-dev.200"),a}),"2.0.5-dev.16");
    check(selected.at("version")=="2.0.5-dev.100" && selected.at("archive_name")=="shin-desktop-2.0.5-dev.100-windows-AMD64.msi","Windows MSI/version selection failed");
    check(selected.at("checksum_url")==selected.at("archive_url").get<std::string>()+".sha256","Windows checksum is not paired");
    check(!select_windows_release(Json::array({a}),"2.0.5-dev.17").at("available"),"same Windows version offered");
    check(!select_windows_release(Json::array({a}),"2.0.4").at("available"),"Windows stable receives preview");
    check(!select_local_release(Json::array({a}),"2.0.5-dev.16").at("available"),"Windows MSI offered to macOS");
    for (std::size_t asset=0;asset<4;++asset) {
        auto r=a;r["assets"].erase(asset);
        check(!select_windows_release(Json::array({r}),"2.0.5-dev.16").at("available"),"incomplete Windows release offered");
        r=a;r["assets"].push_back(r["assets"][asset]);
        check(!select_windows_release(Json::array({r}),"2.0.5-dev.16").at("available"),"ambiguous Windows release offered");
        r=a;r["assets"][asset]["browser_download_url"]="https://example.invalid/file";
        check(!select_windows_release(Json::array({r}),"2.0.5-dev.16").at("available"),"foreign Windows asset offered");
        r=a;r["assets"][asset]["size"]=0;
        check(!select_windows_release(Json::array({r}),"2.0.5-dev.16").at("available"),"empty Windows asset offered");
    }
    for (std::size_t asset:{0U,2U}) {
        auto r=a;r["assets"][asset]["digest"]="sha256:"+std::string(64,'z');
        check(!select_windows_release(Json::array({r}),"2.0.5-dev.16").at("available"),"invalid Windows digest offered");
    }
}
}
int main()try {
    windows_release_selection();
    auto a=release("2.0.5-dev.17"),b=release("2.0.5-dev.100");
    const auto selected=select_local_release(Json::array({a,b,release("2.0.5-dev.9")}),"2.0.5-dev.16");
    check(selected.at("version")=="2.0.5-dev.100","lexical version sorting");
    check(selected.at("revision")==select_local_release(Json::array({b,a}),"2.0.5-dev.16").at("revision"),"offer is order-dependent");
    check(!select_local_release(Json::array({a}),"2.0.5-dev.17").at("available"),"same version offered");
    check(!select_local_release(Json::array({a}),"2.0.4").at("available"),"stable receives preview");
    check(select_local_release(Json::array({release("2.0.5",false)}),"2.0.5-rc.1").at("available"),"preview cannot graduate to stable");
    auto r=a;r["draft"]=true;refused(r);
    r=a;r["prerelease"]=false;refused(r);
    r=a;r["tag_name"]="v2.0.5-dev.017";refused(r);
    r=a;r["assets"].erase(1);refused(r);
    r=a;r["assets"][0]["browser_download_url"]="https://example.invalid/payload";refused(r);
    r=a;r["html_url"]="https://example.invalid/release";refused(r);
    r=a;r["assets"][0]["digest"]="sha256:"+std::string(64,'g');refused(r);
    r=a;r["assets"][0]["size"]=129U*1024*1024;refused(r);
    r=a;r["assets"].push_back(r["assets"][0]);refused(r);
    r=a;r["assets"][0]["state"]="new";refused(r);
    auto changed=a;changed["assets"][0]["digest"]="sha256:"+std::string(64,'b');
    check(select_local_release(Json::array({a}),"2.0.5-dev.16").at("revision")!=select_local_release(Json::array({changed}),"2.0.5-dev.16").at("revision"),"changed archive retained approval");
    auto updater=release("2.0.5-dev.99");
    const std::string updater_url="https://github.com/toseek-samurai/shin-context-engine/releases/download/v2.0.5-dev.99/";
    updater["assets"][0]["name"]=updater_archive_name;updater["assets"][0]["browser_download_url"]=updater_url+updater_archive_name;
    updater["assets"][1]["name"]=std::string(updater_archive_name)+".sha256";updater["assets"][1]["browser_download_url"]=updater_url+updater_archive_name+".sha256";
    check(select_updater_release(Json::array({updater}),"2.0.5-dev.16").at("available"),"updater selection depends on a kit archive");
    check(!select_local_release(Json::array({updater}),"2.0.5-dev.16").at("available"),"updater mistaken for a kit");
    // GitHub's full release metadata exceeds the local-state budget. Both
    // consumers must accept it without changing the offer or parser safeguards.
    auto feed=Json::array({a,updater});
    feed[0]["body"]=std::string(300U*1024U,'x');
    auto text=feed.dump();
    check(text.size()>kMaximumStateBytes && text.size()<kMaximumReleaseFeedBytes,"fixture misses feed-only byte range");
    rejects("json-too-large",[&]{(void)parse_json(text);});
    const auto parsed=parse_release_feed(text);
    check(select_local_release(parsed,"2.0.5-dev.16")==select_local_release(Json::array({a}),"2.0.5-dev.16"),"metadata changed kit offer");
    check(select_updater_release(parsed,"2.0.5-dev.16")==select_updater_release(Json::array({updater}),"2.0.5-dev.16"),"metadata changed updater offer");
    text.resize(kMaximumReleaseFeedBytes,' ');
    check(parse_release_feed(text)==feed,"exact feed byte limit refused");
    rejects("json-too-large",[&]{(void)parse_release_feed(text+" ");});
    const auto padding=std::string(kMaximumStateBytes,' ');
    for(const auto& invalid:std::vector<std::string>{"[{\"draft\":true,\"draft\":false}]", "[{\"asset\":{\"size\":1,\"size\":2}}]",
            std::string(33,'[')+"0"+std::string(33,']'), "[", "[] true"})
        rejects("invalid-json",[&]{(void)parse_release_feed(padding+invalid);});
    rejects("update-release-invalid",[&]{(void)select_local_release(parse_release_feed("{}"),"2.0.5-dev.16");});
    rejects("update-release-invalid",[&]{(void)select_updater_release(Json(std::vector<Json>(31,a)),"2.0.5-dev.16");});
    const auto root=std::string("shin-local-2.0.5-dev.17-macos-arm64");
    const auto listing=root+"/\n"+root+"/shin-kit.json\n"+root+"/bin/shin-ce\n";
    verify_release_listing(listing,"2.0.5-dev.17");
    for(const auto& extra:std::vector<std::string>{"/tmp/escape", "../escape", "wrong/root", root+"/../escape", root+"/bin/shin-ce"}) {
        bool failed=false;try{verify_release_listing(listing+extra+"\n","2.0.5-dev.17");}catch(const Error&){failed=true;}
        check(failed,"unsafe or duplicate archive path accepted");
    }
    std::cout<<"release selection, channel, assets, approval digest and archive paths passed\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
