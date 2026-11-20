cmake_minimum_required(VERSION 3.25)

# ANDOR ARP: one musical input, three independent performances, one DAW track.
# The arpeggiator is ANDOR OSP's (andor_arp_core, from the pinned osp/ submodule): nothing musical
# is copied here. This repository holds the product: the three-lane coordinator, the host adapters
# (Max for Live external, later the JUCE instrument host), their tests and the release material.
#
#   git submodule update --init
#   cmake -B build -G Ninja && cmake --build build && ctest --test-dir build --output-on-failure
#
# The Max external needs the Max SDK (max-sdk-base) and macOS or Windows:
#   cmake -B build-max -G Ninja -DANDOR_ARP_BUILD_MAX_EXTERNAL=ON -DANDOR_ARP_MAX_SDK_DIR=<max-sdk-base>

file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" ANDOR_ARP_VERSION_STRING LIMIT_COUNT 1)
string(REGEX MATCH "^[0-9]+\\.[0-9]+\\.[0-9]+" ANDOR_ARP_VERSION_NUMBER "${ANDOR_ARP_VERSION_STRING}")
project(AndorArp VERSION ${ANDOR_ARP_VERSION_NUMBER} DESCRIPTION "ANDOR ARP" LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()
if(APPLE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")
endif()
if(MSVC)
    add_compile_options(/utf-8)
endif()

option(ANDOR_ARP_BUILD_TESTS "Build ANDOR ARP's tests (and the core's own)" ON)
option(ANDOR_ARP_BUILD_MAX_EXTERNAL "Build the andor.arp.engine Max external (needs the Max SDK)" OFF)
set(ANDOR_ARP_OSP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/osp" CACHE PATH "ANDOR OSP checkout (the pinned submodule)")
set(ANDOR_ARP_CATCH2_DIR "" CACHE PATH "Local Catch2 checkout (skips download)")

if(NOT EXISTS "${ANDOR_ARP_OSP_DIR}/arp-core/CMakeLists.txt")
    message(FATAL_ERROR "ANDOR OSP's arp-core not found at ${ANDOR_ARP_OSP_DIR}: run `git submodule update --init`")
endif()

# ---------------------------------------------------------------------------------------------
# Build identity: the product version and the exact core it was built from, in every binary.
# ---------------------------------------------------------------------------------------------
find_package(Git QUIET)
set(ANDOR_ARP_OSP_COMMIT "unknown")
set(ANDOR_ARP_COMMIT "unknown")
if(GIT_FOUND)
    execute_process(COMMAND ${GIT_EXECUTABLE} -C ${ANDOR_ARP_OSP_DIR} rev-parse HEAD
                    OUTPUT_VARIABLE ANDOR_ARP_OSP_COMMIT OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    execute_process(COMMAND ${GIT_EXECUTABLE} -C ${CMAKE_CURRENT_SOURCE_DIR} rev-parse HEAD
                    OUTPUT_VARIABLE ANDOR_ARP_COMMIT OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    execute_process(COMMAND ${GIT_EXECUTABLE} -C ${ANDOR_ARP_OSP_DIR} status --porcelain --untracked-files=no
                    OUTPUT_VARIABLE _osp_dirty OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(_osp_dirty)
        string(APPEND ANDOR_ARP_OSP_COMMIT "-modified")
    endif()
endif()
configure_file(shared/BuildInfo.h.in ${CMAKE_CURRENT_BINARY_DIR}/generated/BuildInfo.h @ONLY)

# ---------------------------------------------------------------------------------------------
# The shared ARP core, from OSP (never copied). Its own tests (including the 18-style golden)
# run here too, so a dependency update is checked against the core's goldens in this repository.
# ---------------------------------------------------------------------------------------------
if(ANDOR_ARP_BUILD_TESTS)
    if(ANDOR_ARP_CATCH2_DIR)
        add_subdirectory(${ANDOR_ARP_CATCH2_DIR} ${CMAKE_BINARY_DIR}/_deps/catch2-build EXCLUDE_FROM_ALL)
    else()
        include(FetchContent)
        FetchContent_Declare(Catch2 GIT_REPOSITORY https://github.com/catchorg/Catch2.git GIT_TAG v3.9.1 GIT_SHALLOW TRUE)
        FetchContent_MakeAvailable(Catch2)
    endif()
    enable_testing()
endif()
set(ANDOR_ARP_CORE_BUILD_TESTS ${ANDOR_ARP_BUILD_TESTS})
add_subdirectory(${ANDOR_ARP_OSP_DIR}/arp-core andor-arp-core)

function(andor_arp_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion)
    endif()
endfunction()

# ---------------------------------------------------------------------------------------------
# The product's shared layer: pure C++, no host SDK.
# ---------------------------------------------------------------------------------------------
add_library(andor_arp_shared STATIC
    shared/PerformanceModel.cpp
    shared/ThreeLaneCoordinator.cpp
    shared/LinkProtocol.cpp
    shared/StateBlob.cpp
    max/external/MaxSchedulingAdapter.cpp
)
target_include_directories(andor_arp_shared PUBLIC shared max/external ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_link_libraries(andor_arp_shared PUBLIC andor::arp_core)
set_target_properties(andor_arp_shared PROPERTIES POSITION_INDEPENDENT_CODE ON)
andor_arp_warnings(andor_arp_shared)

if(ANDOR_ARP_BUILD_TESTS)
    add_executable(andor_arp_tests
        tests/ThreeLaneCoordinatorTests.cpp
        tests/PerformanceModelTests.cpp
        tests/LinkProtocolTests.cpp
        tests/StateBlobTests.cpp
        tests/MaxSchedulingTests.cpp
        tests/TestRender.cpp
        tests/PresetTests.cpp
        tests/HostGlueTests.cpp
        tests/SchedulerSimulation.cpp
        ${ANDOR_ARP_OSP_DIR}/tests/support/AllocationCounter.cpp   # OSP's real-time check, reused
    )
    target_include_directories(andor_arp_tests PRIVATE ${ANDOR_ARP_OSP_DIR}/tests tests)
    find_package(Threads REQUIRED)
    target_link_libraries(andor_arp_tests PRIVATE andor_arp_shared Catch2::Catch2WithMain Threads::Threads)
    target_compile_definitions(andor_arp_tests PRIVATE ANDOR_ARP_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
                                                       ANDOR_ARP_BINARY_DIR="${CMAKE_CURRENT_BINARY_DIR}")
    andor_arp_warnings(andor_arp_tests)
    add_test(NAME arp-product-unit COMMAND andor_arp_tests "[unit]")
    add_test(NAME arp-product-timing COMMAND andor_arp_tests "[timing]")

    # The reproducible timing report (docs/results): the scheduler simulation's measurements.
    add_executable(andor-arp-timing-report tests/TimingReportMain.cpp tests/SchedulerSimulation.cpp)
    target_link_libraries(andor-arp-timing-report PRIVATE andor_arp_shared)
endif()

if(ANDOR_ARP_BUILD_MAX_EXTERNAL)
    add_subdirectory(max/external)
endif()
