# Windows release profiles historically ship with CRLF. Bindings pin their exact
# bytes, so an LF checkout must produce the same package revision as a native
# Windows checkout. Do not interpolate profile content or change source files.
function(shin_windows_profile_bytes input output)
    file(READ "${input}" content)
    string(REPLACE "\r\n" "\n" content "${content}")
    # file(WRITE) uses native text-mode newlines on Windows. Adding CR there
    # explicitly would produce CRCRLF; other hosts need the explicit CR.
    if(NOT CMAKE_HOST_WIN32)
        string(REPLACE "\n" "\r\n" content "${content}")
    endif()
    file(WRITE "${output}" "${content}")
endfunction()
