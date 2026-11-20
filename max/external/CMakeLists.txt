# andor.arp.engine (.mxo on macOS, .mxe64 on Windows) from the same andor_arp_shared / andor_arp_core
# the tests use. Needs the Max SDK base (MIT licence): -DANDOR_ARP_MAX_SDK_DIR=<max-sdk-base>.
# CI checks out Cycling74/max-sdk-base at the commit pinned in .github/workflows/ci.yml.
#
# On Linux (no Max) the object is still compiled (LINUX_VERSION headers) as a syntax and type
# check of the adapter; it is not linked, as Max provides its API at run time only on macOS/Windows.

set(ANDOR_ARP_MAX_SDK_DIR "" CACHE PATH "Cycling74/max-sdk-base checkout")
if(NOT EXISTS "${ANDOR_ARP_MAX_SDK_DIR}/c74support/max-includes/ext.h")
    message(FATAL_ERROR "ANDOR_ARP_MAX_SDK_DIR must point at a max-sdk-base checkout")
endif()
set(_c74 "${ANDOR_ARP_MAX_SDK_DIR}/c74support")

if(APPLE)
    add_library(andor.arp.engine MODULE andor.arp.engine.cpp)
    target_compile_definitions(andor.arp.engine PRIVATE MAC_VERSION C74_NO_DEPRECATION)
    # As the SDK's own max-pretarget.cmake: Max's symbols stay undefined at link time (Max provides
    # them when it loads the external).
    file(STRINGS "${ANDOR_ARP_MAX_SDK_DIR}/script/max-linker-flags.txt" _max_linker_flags)
    separate_arguments(_max_linker_flags UNIX_COMMAND "${_max_linker_flags}")
    target_link_options(andor.arp.engine PRIVATE ${_max_linker_flags})
    set_target_properties(andor.arp.engine PROPERTIES
        BUNDLE TRUE
        BUNDLE_EXTENSION "mxo"
        MACOSX_BUNDLE_INFO_PLIST "${ANDOR_ARP_MAX_SDK_DIR}/script/Info.plist.in"
        MACOSX_BUNDLE_BUNDLE_NAME "andor.arp.engine"
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.andor.arp.engine"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        XCODE_ATTRIBUTE_WRAPPER_EXTENSION "mxo"
        PREFIX "")
    add_custom_command(TARGET andor.arp.engine POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy "${ANDOR_ARP_MAX_SDK_DIR}/script/PkgInfo" "$<TARGET_BUNDLE_CONTENT_DIR:andor.arp.engine>/PkgInfo")
elseif(WIN32)
    add_library(andor.arp.engine MODULE andor.arp.engine.cpp)
    target_compile_definitions(andor.arp.engine PRIVATE WIN_VERSION WIN_EXT_VERSION _CRT_SECURE_NO_WARNINGS C74_NO_DEPRECATION)
    target_link_libraries(andor.arp.engine PRIVATE "${_c74}/max-includes/x64/MaxAPI.lib")
    set_target_properties(andor.arp.engine PROPERTIES SUFFIX ".mxe64")
else()
    add_library(andor.arp.engine OBJECT andor.arp.engine.cpp)
    target_compile_definitions(andor.arp.engine PRIVATE LINUX_VERSION C74_NO_DEPRECATION)
endif()
target_include_directories(andor.arp.engine SYSTEM PRIVATE "${_c74}" "${_c74}/max-includes")
target_link_libraries(andor.arp.engine PRIVATE andor_arp_shared)
set_target_properties(andor.arp.engine PROPERTIES CXX_VISIBILITY_PRESET hidden)
