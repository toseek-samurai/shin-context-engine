#include "shin/windows/transport.h"
#include <windows.h>
#include <cassert>
#include <chrono>
#include <iostream>

int main() try {
    using namespace shin::windows;
    const std::string key(64,'d');
    bool denied=false;
    try { auto listener=PipeListener::system_broker(key); }
    catch(const std::exception&) { denied=true; }
    assert(denied); // An ordinary process cannot create the system endpoint.
    const std::wstring name=L"\\\\.\\pipe\\shin-system-v1-"+std::wstring(key.begin(),key.end());
    HANDLE fake=CreateNamedPipeW(name.c_str(),PIPE_ACCESS_DUPLEX|FILE_FLAG_OVERLAPPED|FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE|PIPE_READMODE_BYTE|PIPE_WAIT|PIPE_REJECT_REMOTE_CLIENTS,1,4096,4096,0,nullptr);
    assert(fake!=INVALID_HANDLE_VALUE);
    denied=false;
    try { auto client=Channel::connect_system(key,GetCurrentProcessId(),std::chrono::steady_clock::now()+std::chrono::seconds(1)); }
    catch(const std::exception&) { denied=true; }
    CloseHandle(fake);
    assert(denied); // Matching name and claimed PID cannot replace SYSTEM ownership.
    PipeListener private_listener(key);
    auto client=Channel::connect(key,std::chrono::steady_clock::now()+std::chrono::seconds(1));
    auto server=private_listener.accept(std::chrono::steady_clock::now()+std::chrono::seconds(1));
    assert(server.peer_sid().empty()); // Private project transport has no cross-user opt-in.
    std::cout<<"SCM transport: unprivileged listener and fake system endpoint refused; private route unchanged\n";
    return 0;
} catch(const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
