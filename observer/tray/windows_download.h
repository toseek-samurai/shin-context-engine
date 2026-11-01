#pragma once
#include <windows.h>
#include <winhttp.h>
#include "shin/observer/local_updates.h"
#include <array>
#include <chrono>
#include <memory>
#include <sstream>
#include <vector>

namespace shin::observer::windows_update {
inline void require(bool valid) { if(!valid) throw Error("update-download-invalid","The release download could not be verified."); }
inline std::wstring wide_url(const std::string& value) {
    require(value.size()<=16384);
    const int size=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),nullptr,0);
    require(size>0); std::wstring result(static_cast<std::size_t>(size),0);
    require(MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),result.data(),size)>0); return result;
}
inline std::string download(std::wstring url,std::size_t limit,const std::function<void(std::uint64_t)>& progress={}) {
    struct Close { void operator()(void* handle) const {if(handle) WinHttpCloseHandle(handle);} };
    using Internet=std::unique_ptr<void,Close>;
    Internet session(WinHttpOpen(L"Shin-Updater/1",WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,0));
    require(session!=nullptr); require(WinHttpSetTimeouts(session.get(),5000,5000,5000,5000)!=FALSE);
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(120);
    for(int hop=0;hop<5;++hop) {
        URL_COMPONENTS parts{}; parts.dwStructSize=sizeof(parts);
        parts.dwHostNameLength=DWORD(-1);parts.dwUrlPathLength=DWORD(-1);parts.dwExtraInfoLength=DWORD(-1);
        parts.dwUserNameLength=DWORD(-1);parts.dwPasswordLength=DWORD(-1);
        require(WinHttpCrackUrl(url.c_str(),static_cast<DWORD>(url.size()),0,&parts)!=FALSE);
        const std::wstring host(parts.lpszHostName,parts.dwHostNameLength);
        require(parts.nScheme==INTERNET_SCHEME_HTTPS && parts.nPort==443 && !parts.dwUserNameLength && !parts.dwPasswordLength);
        require(host==L"github.com" || host==L"api.github.com" || host==L"release-assets.githubusercontent.com" || host==L"objects.githubusercontent.com");
        std::wstring path(parts.lpszUrlPath,parts.dwUrlPathLength);
        if(parts.dwExtraInfoLength) path.append(parts.lpszExtraInfo,parts.dwExtraInfoLength);
        Internet connection(WinHttpConnect(session.get(),host.c_str(),443,0));require(connection!=nullptr);
        Internet request(WinHttpOpenRequest(connection.get(),L"GET",path.c_str(),nullptr,WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,WINHTTP_FLAG_SECURE));require(request!=nullptr);
        DWORD redirects=WINHTTP_OPTION_REDIRECT_POLICY_NEVER, disabled=WINHTTP_DISABLE_COOKIES|WINHTTP_DISABLE_AUTHENTICATION;
        require(WinHttpSetOption(request.get(),WINHTTP_OPTION_REDIRECT_POLICY,&redirects,sizeof(redirects))!=FALSE);
        require(WinHttpSetOption(request.get(),WINHTTP_OPTION_DISABLE_FEATURE,&disabled,sizeof(disabled))!=FALSE);
        require(WinHttpSendRequest(request.get(),WINHTTP_NO_ADDITIONAL_HEADERS,0,WINHTTP_NO_REQUEST_DATA,0,0,0)!=FALSE);
        require(WinHttpReceiveResponse(request.get(),nullptr)!=FALSE);
        DWORD status=0,size=sizeof(status);
        require(WinHttpQueryHeaders(request.get(),WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,WINHTTP_HEADER_NAME_BY_INDEX,&status,&size,WINHTTP_NO_HEADER_INDEX)!=FALSE);
        if(status==301 || status==302 || status==303 || status==307 || status==308) {
            size=0; WinHttpQueryHeaders(request.get(),WINHTTP_QUERY_LOCATION,WINHTTP_HEADER_NAME_BY_INDEX,nullptr,&size,WINHTTP_NO_HEADER_INDEX);
            require(size>0 && size<=32768);std::vector<wchar_t> location(size/sizeof(wchar_t)+1);
            require(WinHttpQueryHeaders(request.get(),WINHTTP_QUERY_LOCATION,WINHTTP_HEADER_NAME_BY_INDEX,location.data(),&size,WINHTTP_NO_HEADER_INDEX)!=FALSE);
            url=location.data();require(std::chrono::steady_clock::now()<deadline);continue;
        }
        require(status==200);std::string bytes;std::array<char,65536> buffer{};
        for(;;) {
            require(std::chrono::steady_clock::now()<deadline);DWORD read=0;
            require(WinHttpReadData(request.get(),buffer.data(),static_cast<DWORD>(buffer.size()),&read)!=FALSE);
            if(!read) return bytes;
            require(bytes.size()+read<=limit);bytes.append(buffer.data(),read);if(progress) progress(bytes.size());
        }
    }
    throw Error("update-download-invalid","Too many release redirects.");
}
inline void verify(const Json& offer,const std::string& payload,const std::string& checksum) {
    require(payload.size()==offer.at("bytes").get<std::uint64_t>() && digest(payload)==offer.at("sha256").get_ref<const std::string&>());
    std::istringstream input(checksum);std::string hash,name,extra;input>>hash>>name;
    if(name.starts_with('*')) name.erase(0,1);
    require(hash==offer.at("sha256").get_ref<const std::string&>() && name==offer.at("archive_name").get_ref<const std::string&>() && !(input>>extra));
}
}
