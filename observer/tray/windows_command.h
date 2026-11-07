#pragma once
// Implementation fragment included inside each host's private namespace.
// Its shin/windows/file.h and ui/project_progress.h dependencies must be
// included at translation-unit scope so their namespaces remain global.

// The UI invokes the matching native CLI directly, never through a shell.
// Drain both streams with fixed output/deadline bounds. The CLI owns its worker
// jobs; an intentionally detached service must survive completion of this command.
Json windows_command(const fs::path& executable, const std::vector<std::string>& arguments,
                     std::chrono::seconds timeout = std::chrono::seconds(120),
                     const std::function<void(int)>& progress = {}) {
    struct Handle {
        HANDLE value = nullptr;
        ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    } out_read, out_write, err_read, err_write, input, process, thread;
    shin::windows::ReadFile image(executable); image.require_executable_owner();
    const auto quote = [](const std::wstring& argument) {
        std::wstring result = L"\""; std::size_t slashes = 0;
        for (const auto c : argument) {
            if (c == L'\\') { ++slashes; continue; }
            result.append(slashes * (c == wchar_t{34} ? 2 : 1), L'\\'); slashes = 0;
            if (c == wchar_t{34}) result += L'\\';
            result += c;
        }
        result.append(slashes * 2, L'\\'); result += wchar_t{34}; return result;
    };
    std::wstring command = quote(executable.wstring());
    for (const auto& argument : arguments) command += L" " + quote(wide(argument));
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    if (!CreatePipe(&out_read.value, &out_write.value, &security, 0) ||
        !CreatePipe(&err_read.value, &err_write.value, &security, 0) ||
        !SetHandleInformation(out_read.value, HANDLE_FLAG_INHERIT, 0) ||
        !SetHandleInformation(err_read.value, HANDLE_FLAG_INHERIT, 0))
        throw Error("monitor-command-failed", "Cannot prepare native command output.");
    input.value = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING, 0, nullptr);
    if (input.value == INVALID_HANDLE_VALUE) throw Error("monitor-command-failed", "Cannot prepare native command input.");
    SIZE_T size = 0; InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    std::vector<unsigned char> attributes(size);
    auto* list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
    if (!InitializeProcThreadAttributeList(list, 1, 0, &size)) throw Error("monitor-command-failed", "Cannot prepare native command handles.");
    struct Attributes { LPPROC_THREAD_ATTRIBUTE_LIST value; ~Attributes(){ DeleteProcThreadAttributeList(value); } } cleanup{list};
    HANDLE handles[]{input.value, out_write.value, err_write.value};
    if (!UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles, sizeof(handles), nullptr, nullptr))
        throw Error("monitor-command-failed", "Cannot isolate native command handles.");
    STARTUPINFOEXW startup{}; startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = input.value; startup.StartupInfo.hStdOutput = out_write.value; startup.StartupInfo.hStdError = err_write.value;
    startup.lpAttributeList = list;
    PROCESS_INFORMATION child{};
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT, nullptr, executable.parent_path().c_str(), &startup.StartupInfo, &child))
        throw Error("monitor-command-failed", "Cannot start the matching native CLI.");
    process.value = child.hProcess; thread.value = child.hThread;
    struct Running { HANDLE value; ~Running() { if (WaitForSingleObject(value,0)==WAIT_TIMEOUT) TerminateProcess(value,1); } } running{process.value};
    if (ResumeThread(thread.value) == DWORD(-1)) throw Error("monitor-command-failed", "Cannot resume native command.");
    CloseHandle(out_write.value); out_write.value = nullptr; CloseHandle(err_write.value); err_write.value = nullptr;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::string output, errors;
    shin::ui::ProjectProgressReader project_progress;
    const auto drain = [&](HANDLE handle, std::string& bytes, bool diagnostic) {
        DWORD available = 0;
        if (!PeekNamedPipe(handle, nullptr, 0, nullptr, &available, nullptr)) return;
        std::array<char, 8192> buffer{};
        while (available) {
            DWORD read = 0;
            if (!ReadFile(handle, buffer.data(), std::min(available, static_cast<DWORD>(buffer.size())), &read, nullptr) || !read) break;
            if (bytes.size() + read > kMaximumStateBytes) throw Error("monitor-response-too-large", "Native command output exceeds its bound.");
            bytes.append(buffer.data(), read); available -= read;
            if (diagnostic && progress) project_progress.feed(std::string_view(buffer.data(), read), progress);
        }
    };
    for (;;) {
        drain(out_read.value, output, false); drain(err_read.value, errors, true);
        if (WaitForSingleObject(process.value, 20) == WAIT_OBJECT_0) break;
        if (std::chrono::steady_clock::now() >= deadline) throw Error("monitor-command-timeout", "Native command exceeded its deadline. Check project status before retrying.");
    }
    drain(out_read.value, output, false); drain(err_read.value, errors, true);
    DWORD code = 1; GetExitCodeProcess(process.value, &code);
    Json result = parse_json(output.empty() ? (progress ? project_progress.finish() : errors) : output);
    if (!result.is_object() || !result.contains("ok") || !result.at("ok").is_boolean())
        throw Error("monitor-response-invalid", "Native command returned an invalid response.");
    if (code && result.value("ok", false)) throw Error("monitor-response-invalid", "Native command did not confirm completion.");
    return result;
}
