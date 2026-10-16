if(CPACK_GENERATOR STREQUAL "WIX" AND CMAKE_VERSION VERSION_LESS 4.3)
    message(FATAL_ERROR "The qualified Windows MSI recipe requires CPack 4.3 or later")
endif()
