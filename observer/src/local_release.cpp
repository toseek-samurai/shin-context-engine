#include "shin/observer/local_updates.h"
#include "shin/observer/updater_bundle.h"
#include "shin/observer/observer.h"
#include <array>
#include <regex>
#include <set>
#include <sstream>
namespace shin::observer {
namespace {
using Version = std::array<unsigned long, 5>;
std::optional<Version> version(const std::string& text) {
    static const std::regex pattern(R"(^((0|[1-9][0-9]{0,8}))\.((0|[1-9][0-9]{0,8}))\.((0|[1-9][0-9]{0,8}))(?:-(dev|rc)\.((0|[1-9][0-9]{0,8})))?$)");
    std::smatch match;
    if (!std::regex_match(text, match, pattern)) return {};
    return Version{std::stoul(match[1]),std::stoul(match[3]),std::stoul(match[5]),
        match[7] == "dev" ? 0UL : match[7] == "rc" ? 1UL : 2UL,
        match[8].matched ? std::stoul(match[8]) : 0UL};
}
void require(bool ok) { if (!ok) throw Error("update-release-invalid", "The release metadata or archive is not a complete supported kit."); }
}
namespace {
Json select_release(const Json& releases, const std::string& current, bool updater) {
    const auto installed=version(current);
    require(installed.has_value() && releases.is_array() && releases.size() <= 30);
    Json result{{"ok",true},{"current_version",current},{"available",false},{"channel",(*installed)[3] == 2 ? "stable" : "preview"}};
    auto newest=*installed;
    for (const auto& release:releases) {
        try {
            if (!release.is_object() || release.at("draft").get<bool>()) continue;
            const auto tag=release.at("tag_name").get<std::string>();
            if (tag.empty() || tag.front()!='v') continue;
            const auto next=version(tag.substr(1));
            if (!next || *next<newest || (!updater && *next==newest) || (release.at("prerelease").get<bool>() != ((*next)[3]!=2)) ||
                ((*installed)[3]==2 && (*next)[3]!=2)) continue;
            const auto page="https://github.com/toseek-samurai/shin-context-engine/releases/tag/"+tag;
            if (release.at("html_url")!=page || !release.at("assets").is_array() || release.at("assets").size()>20) continue;
            const auto name=updater ? std::string(updater_archive_name) : "shin-local-"+tag.substr(1)+"-macos-arm64.zip";
            const auto download="https://github.com/toseek-samurai/shin-context-engine/releases/download/"+tag+"/";
            Json archive,checksum;
            bool duplicate=false;
            for (const auto& asset:release.at("assets")) {
                if (asset.value("name","")!=name && asset.value("name","")!=name+".sha256") continue;
                auto& destination=asset.at("name")==name ? archive : checksum;
                if (!destination.is_null()) {duplicate=true;break;}
                destination=asset;
            }
            if (duplicate || archive.is_null() || checksum.is_null()) continue;
            const auto hash=archive.at("digest").get<std::string>();
            if (hash.size()!=71 || hash.substr(0,7)!="sha256:" || hash.substr(7).find_first_not_of("0123456789abcdef")!=std::string::npos ||
                archive.at("state")!="uploaded" || checksum.at("state")!="uploaded" ||
                archive.at("browser_download_url")!=download+name || checksum.at("browser_download_url")!=download+name+".sha256") continue;
            const auto bytes=archive.at("size").get<std::uint64_t>();
            if (!bytes || bytes>(updater ? updater_archive_limit : 128U*1024*1024) || checksum.at("size").get<std::uint64_t>()>512) continue;
            newest=*next;
            result.update({{"available",true},{"version",tag.substr(1)},{"release_url",page},{"archive_url",download+name},
                {"checksum_url",download+name+".sha256"},{"archive_name",name},{"sha256",hash.substr(7)},{"bytes",bytes}});
        } catch (const Json::exception&) { continue; }
    }
    if (result.at("available")==true) result["revision"]=digest(result.dump());
    return result;
}
} // namespace
Json parse_release_feed(const std::string& text) { return parse_json(text, kMaximumReleaseFeedBytes); }
Json select_local_release(const Json& releases, const std::string& current) { return select_release(releases, current, false); }
Json select_updater_release(const Json& releases, const std::string& current) { return select_release(releases, current, true); }
void verify_release_listing(const std::string& listing,const std::string& release) {
    require(version(release).has_value() && listing.size()<=256U*1024);
    const std::string root="shin-local-"+release+"-macos-arm64";
    std::set<std::string> seen;std::istringstream input(listing);std::string line;
    while (std::getline(input,line)) {
        require(!line.empty() && line.size()<=2048 && line.find_first_of("\\\r\t") == std::string::npos && line.find('\0')==std::string::npos);
        if (line.back()=='/') line.pop_back();
        const fs::path path(line);
        require(!path.is_absolute() && path.lexically_normal()==path &&
            (line==root || line.starts_with(root+"/")) && seen.insert(line).second && seen.size()<=2048);
        for (const auto& part:path) require(part!=".." && part!="." && !part.empty());
    }
    require(seen.contains(root+"/shin-kit.json") && seen.contains(root+"/bin/shin-ce"));
}
} // namespace shin::observer
