#pragma once
#include "shin/observer/observer.h"
#include "shin/windows/file.h"
#include <windows.h>
#include <array>
#include <functional>
#include <set>
#include <cctype>
#include <algorithm>

namespace shin::observer::windows_update {
inline fs::path msi_install_root(fs::path path) {
    // Windows Installer directory properties end in a separator. Remove only
    // that spelling detail; native IO must still reject aliases and reparse paths.
    if(path!=path.root_path() && path.filename().empty()) path=path.parent_path();
    return path;
}
inline void verify_msi_install_root(const fs::path& prefix,const fs::path& registered) {
    if(directory_identity(prefix)!=directory_identity(msi_install_root(registered)))
        throw Error("update-msi-installation-required","This route updates the registered MSI installation.");
}
inline void verify_registered_msi_installation(const fs::path& prefix) {
    std::array<wchar_t,32768> value{};DWORD bytes=static_cast<DWORD>(sizeof(value));
    if(RegGetValueW(HKEY_LOCAL_MACHINE,L"Software\\Shin\\DesktopPreview",L"InstallRoot",RRF_RT_REG_SZ|RRF_SUBKEY_WOW6464KEY,nullptr,value.data(),&bytes)!=ERROR_SUCCESS)
        throw Error("update-msi-installation-required","This route updates the registered MSI installation.");
    verify_msi_install_root(prefix,fs::path(value.data()));
}
inline Json stage_update_helper(const fs::path& prefix,const fs::path& folder) {
    const auto bin=prefix/"bin";
    shin::windows::ReadFile helper(bin/"shin-windows-updater.exe");helper.require_machine_owned();
    shin::windows::Directory::ensure_private(folder,true);
    shin::windows::Directory destination(folder,shin::windows::Directory::Access::private_owner);
    if(!destination.create("updater.exe",helper.read(64U*1024*1024))) throw Error("update-stage-conflict","Update helper already exists.");
    unsigned files=0;
    for(const auto& entry:fs::directory_iterator(bin)) if(entry.path().extension()==".dll") {
        if(++files>16) throw Error("update-stage-invalid","Too many helper libraries.");
        shin::windows::ReadFile library(entry.path());library.require_machine_owned();
        if(!destination.create(entry.path().filename().string(),library.read(32U*1024*1024))) throw Error("update-stage-conflict","Update helper library already exists.");
    }
    return {{"helper_staged",true},{"libraries",files}};
}
inline void restore_update_monitor(bool& pending,const std::function<void()>& verify,const std::function<void()>& launch) {
    if(!pending) return;
    verify();
    launch();
    pending=false;
}
// Check the complete bounded inventory before reading any payload path.
inline void validate_inventory(const Json& manifest,const std::string& version) {
    const auto require=[](bool valid) {
        if(!valid) throw Error("update-outcome-unknown","Installed inventory could not be confirmed.");
    };
    require(manifest.value("schema_version","")=="shin.local-kit.v1" && manifest.at("version")==version &&
        manifest.at("files").is_array() && manifest.at("files").size()<=2048);
    std::set<std::string> names;
    std::uint64_t total=0;
    for(const auto& entry:manifest.at("files")) {
        const auto name=entry.at("path").get<std::string>();const fs::path relative(name);
        require(!relative.empty() && !relative.has_root_path() && relative.lexically_normal()==relative && name.find(':')==std::string::npos);
        for(const auto& part:relative) require(part!="." && part!="..");
        // Inventory names have one portable spelling, including case. This also
        // excludes NTFS alternate streams, trailing-dot/space aliases and duplicates.
        require(name==relative.generic_string());
        for(const auto& part:relative) {
            const auto component=part.string();
            require(!component.empty() && component.back()!='.' && component.back()!=' ');
        }
        auto key=name;std::transform(key.begin(),key.end(),key.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});
        require(names.insert(key).second);
        const auto bytes=entry.at("size").get<std::uint64_t>();
        require(bytes<=128U*1024*1024 && total<=512U*1024*1024-bytes);total+=bytes;
        const auto hash=entry.at("sha256").get<std::string>();
        require(hash.size()==64 && std::all_of(hash.begin(),hash.end(),[](char c){return (c>='0'&&c<='9')||(c>='a'&&c<='f');}));
    }
    for(const auto* file:{"bin/shin-ce.exe","bin/shin-context.exe","bin/shin-observer.exe","bin/shin-mcp.exe","bin/shin-tray.exe","bin/shin-windows-updater.exe"})
        require(names.contains(file));
}
inline Json verify_installed(const fs::path& prefix,const std::string& version) {
    shin::windows::ReadFile inventory(prefix/"shin-kit.json");inventory.require_machine_owned();
    const auto manifest=parse_json(inventory.read(kMaximumStateBytes));validate_inventory(manifest,version);
    for(const auto& entry:manifest.at("files")) {
        shin::windows::ReadFile file(prefix/fs::path(entry.at("path").get<std::string>()));file.require_machine_owned();
        const auto bytes=entry.at("size").get<std::size_t>();
        if(file.stamp().bytes!=bytes || digest(file.read(bytes))!=entry.at("sha256").get_ref<const std::string&>())
            throw Error("update-outcome-unknown","Installed files could not be confirmed.");
    }
    return manifest;
}
}
