#pragma once
// Implementation fragment included inside each host's private namespace.
// Its shin/windows/file.h and ui/project_progress.h dependencies must be
// included at translation-unit scope so their namespaces remain global.

// The UI invokes the matching native CLI directly, never through a shell.
// Drain both streams with fixed output/deadline bounds. The CLI owns its worker
// jobs; an intentionally detached service must survive completion of this command.
Json windows_command(const fs::path& executable, const std::vector<std::string>& arguments,
                     std::chrono::seconds timeout = std::chrono::seconds(120),
                     const std::function<void(int)>& progress = {}, DWORD* completion_code = nullptr,
                     std::string_view input_bytes = {}) {
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
    if (input_bytes.empty()) {
        input.value = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING, 0, nullptr);
    } else {
        if (input_bytes.size() > kMaximumStateBytes)
            throw Error("client-config-limit", "The selected settings list exceeds its size limit.");
        const auto path = fs::temp_directory_path()/(L"shin-client-"+wide(digest(std::to_string(GetCurrentProcessId())+
            ":"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())).substr(0,24))+L".tmp");
        PSECURITY_DESCRIPTOR raw = nullptr;
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;FA;;;OW)", SDDL_REVISION_1, &raw, nullptr))
            throw Error("monitor-command-failed", "Cannot protect native command input.");
        std::unique_ptr<void,decltype(&LocalFree)> descriptor(raw,&LocalFree);
        SECURITY_ATTRIBUTES private_input{sizeof(private_input),descriptor.get(),FALSE};
        Handle staged;
        staged.value = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE | DELETE, FILE_SHARE_READ | FILE_SHARE_DELETE,
            &private_input, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (staged.value == INVALID_HANDLE_VALUE)
            throw Error("monitor-command-failed", "Cannot prepare native command input.");
        DWORD written = 0;
        const auto bytes = static_cast<DWORD>(input_bytes.size());
        if (!WriteFile(staged.value, input_bytes.data(), bytes, &written, nullptr) || written != bytes ||
            !SetFilePointerEx(staged.value, {}, nullptr, FILE_BEGIN) ||
            !DuplicateHandle(GetCurrentProcess(), staged.value, GetCurrentProcess(), &input.value, GENERIC_READ, TRUE, 0))
            throw Error("monitor-command-failed", "Cannot prepare native command input.");
    }
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
    CloseHandle(input.value); input.value = nullptr;
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
    if (completion_code) *completion_code = code;
    Json result = parse_json(output.empty() ? (progress ? project_progress.finish() : errors) : output);
    if (!result.is_object() || !result.contains("ok") || !result.at("ok").is_boolean())
        throw Error("monitor-response-invalid", "Native command returned an invalid response.");
    if (code && result.value("ok", false)) throw Error("monitor-response-invalid", "Native command did not confirm completion.");
    return result;
}

inline Json prepare_installed_windows_projects(const fs::path& prefix, const fs::path& home,
                                               const Json& manifest) {
    const auto executable = prefix/"bin/shin-ce.exe";
    shin::windows::ReadFile cli(executable); cli.require_machine_owned();
    auto result = windows_command(executable, {"prepare-projects", "--home", home.string()}, std::chrono::seconds(1105));
    if (!result.value("ok", false) && result.value("schema_version", "") == "shin.ce-response.v1" &&
        !result.contains("version") && !result.contains("core_version"))
        throw Error(result.value("error", Json::object()).value("code", "installation-preparation-unavailable"),
            "The installed CLI could not prepare projects.");
    if (result.value("version", "") != manifest.at("version").get_ref<const std::string&>() ||
        result.value("core_version", "") != manifest.at("core_version").get_ref<const std::string&>())
        throw Error("update-installation-changed", "Project preparation belongs to another installation.");
    return result;
}
