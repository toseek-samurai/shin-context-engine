if(CPACK_GENERATOR STREQUAL "WIX")
    if(CMAKE_VERSION VERSION_LESS 4.3)
        message(FATAL_ERROR "The qualified Windows MSI recipe requires CPack 4.3 or later")
    endif()
    # Keep CPack's installed WiX 4 template, changing only its permissive
    # same-version upgrade default. A dependency change needs explicit review.
    file(READ "${CMAKE_ROOT}/Modules/Internal/CPack/WIX.template.in" template)
    string(REGEX MATCHALL "AllowSameVersionUpgrades=\"yes\"" matches "${template}")
    list(LENGTH matches count)
    if(NOT count EQUAL 1 OR NOT template MATCHES "http://wixtoolset.org/schemas/v4/wxs"
       OR NOT IS_ABSOLUTE "${CPACK_SHIN_WIX_TEMPLATE}")
        message(FATAL_ERROR "CPack WiX template changed; review the Shin upgrade policy")
    endif()
    string(REPLACE "AllowSameVersionUpgrades=\"yes\"" "AllowSameVersionUpgrades=\"no\"" template "${template}")
    file(WRITE "${CPACK_SHIN_WIX_TEMPLATE}" "${template}")
    set(CPACK_WIX_TEMPLATE "${CPACK_SHIN_WIX_TEMPLATE}")
endif()
