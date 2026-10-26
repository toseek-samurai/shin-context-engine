#include "shin/observer/local_updates.h"
#include "shin/observer/updater_bundle.h"
#include <iostream>
using namespace shin::observer;
namespace {
void check(bool ok,const char* reason){if(!ok)throw std::runtime_error(reason);}
Json release(const std::string& version,bool preview=true) {
    const auto tag="v"+version,name="shin-local-"+version+"-macos-arm64.zip";
    const std::string base="https://github.com/toseek-samurai/shin-context-engine/releases/";
    return {{"tag_name",tag},{"draft",false},{"prerelease",preview},{"html_url",base+"tag/"+tag},{"assets",Json::array({
        {{"name",name},{"state","uploaded"},{"size",1000},{"digest","sha256:"+std::string(64,'a')},{"browser_download_url",base+"download/"+tag+"/"+name}},
        {{"name",name+".sha256"},{"state","uploaded"},{"size",110},{"browser_download_url",base+"download/"+tag+"/"+name+".sha256"}}
    })}};
}
void refused(Json r){check(!select_local_release(Json::array({r}),"2.0.5-dev.16").at("available"),"unsafe release offered");}
}
int main()try {
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
    const auto root=std::string("shin-local-2.0.5-dev.17-macos-arm64");
    const auto listing=root+"/\n"+root+"/shin-kit.json\n"+root+"/bin/shin-ce\n";
    verify_release_listing(listing,"2.0.5-dev.17");
    for(const auto& extra:std::vector<std::string>{"/tmp/escape", "../escape", "wrong/root", root+"/../escape", root+"/bin/shin-ce"}) {
        bool failed=false;try{verify_release_listing(listing+extra+"\n","2.0.5-dev.17");}catch(const Error&){failed=true;}
        check(failed,"unsafe or duplicate archive path accepted");
    }
    std::cout<<"release selection, channel, assets, approval digest and archive paths passed\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
