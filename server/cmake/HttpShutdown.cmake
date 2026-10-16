# Keep the pinned dependency and control builds immutable. Only Server uses this
# generated header; Observer's client transport does not need this server fix.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${shin_httplib_SOURCE_DIR}/httplib.h")
file(READ "${shin_httplib_SOURCE_DIR}/httplib.h" SHIN_HTTP_HEADER)
string(SHA256 SHIN_HTTP_SHA "${SHIN_HTTP_HEADER}")
if(NOT SHIN_HTTP_SHA STREQUAL "5933c14b2d0f45212925ed18ca579841f5fce717f431fc20cec712423e905b10")
    message(FATAL_ERROR "cpp-httplib header differs from the reviewed v0.54.1 shutdown control")
endif()
set(SHIN_HTTP_BEFORE [=[  // Avoid expensive `steady_clock::now()` call for the first time
  if (select_read(sock, 0, interval_usec) > 0) { return true; }]=])
set(SHIN_HTTP_AFTER [=[  // Do not begin queued/keep-alive requests after listener shutdown, even
  // when a peer already supplied bytes. Active request I/O keeps its timeout.
  if (svr_sock == INVALID_SOCKET) { return false; }
  // Avoid expensive `steady_clock::now()` call for the first time
  if (select_read(sock, 0, interval_usec) > 0) {
    return svr_sock != INVALID_SOCKET;
  }]=])
string(FIND "${SHIN_HTTP_HEADER}" "${SHIN_HTTP_BEFORE}" SHIN_HTTP_MATCH)
if(SHIN_HTTP_MATCH EQUAL -1)
    message(FATAL_ERROR "Pinned cpp-httplib shutdown patch no longer matches; review the dependency")
endif()
string(REPLACE "${SHIN_HTTP_BEFORE}" "${SHIN_HTTP_AFTER}" SHIN_HTTP_HEADER "${SHIN_HTTP_HEADER}")
set(SHIN_SERVER_HTTP_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/http")
file(MAKE_DIRECTORY "${SHIN_SERVER_HTTP_DIR}")
file(CONFIGURE OUTPUT "${SHIN_SERVER_HTTP_DIR}/httplib.h" CONTENT "${SHIN_HTTP_HEADER}" @ONLY)
