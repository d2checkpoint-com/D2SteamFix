cmake_minimum_required(VERSION 3.24)

project(D2SteamFix VERSION 1.1.1 LANGUAGES CXX)

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(NOT WIN32 OR NOT MSVC)
    message(FATAL_ERROR "D2SteamFix requires 64-bit Windows and Microsoft Visual C++.")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "D2SteamFix supports only 64-bit Windows.")
endif()

option(STEAMFIX_ENABLE_CODE_ANALYSIS "Run Microsoft C++ Core Check during the build" OFF)
set(STEAMFIX_CODE_ANALYSIS_RULESET "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CoreGuidelines.ruleset"
    CACHE STRING "Microsoft native code analysis ruleset")
if(STEAMFIX_ENABLE_CODE_ANALYSIS AND NOT CMAKE_GENERATOR MATCHES "^Visual Studio")
    message(FATAL_ERROR "STEAMFIX_ENABLE_CODE_ANALYSIS requires a Visual Studio generator.")
endif()

function(steamfix_enable_code_analysis target)
    if(STEAMFIX_ENABLE_CODE_ANALYSIS)
        set_target_properties(${target} PROPERTIES
            VS_GLOBAL_EnableCppCoreCheck true
            VS_GLOBAL_RunCodeAnalysis true
            VS_GLOBAL_CodeAnalysisRuleSet "${STEAMFIX_CODE_ANALYSIS_RULESET}"
            VS_GLOBAL_CodeAnalysisTreatWarningsAsErrors true
        )
    endif()
endfunction()

configure_file("${CMAKE_CURRENT_SOURCE_DIR}/src/steamfix.rc.in"
               "${CMAKE_CURRENT_BINARY_DIR}/steamfix.rc" @ONLY)

set(STEAMFIX_SOURCES
    src/acl.cpp
    src/app.cpp
    src/elevation.cpp
    src/command_line.cpp
    src/logging.cpp
    src/overlay.cpp
    src/paths.cpp
    src/process.cpp
    src/repair.cpp
    src/steam.cpp
    src/ui.cpp
)
set(STEAMFIX_LIBRARIES advapi32 comctl32 comdlg32 crypt32 ole32 shell32 user32)
add_executable(steamfix WIN32
    src/main.cpp
    ${STEAMFIX_SOURCES}
    "${CMAKE_CURRENT_BINARY_DIR}/steamfix.rc"
    src/steamfix.manifest
)
target_include_directories(steamfix PRIVATE include)
target_compile_features(steamfix PRIVATE cxx_std_20)
target_compile_definitions(steamfix PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    STEAMFIX_VERSION="${PROJECT_VERSION}"
)
target_compile_options(steamfix PRIVATE
    /W4 /WX /permissive- /EHsc /utf-8 /sdl /guard:cf /guard:ehcont /ZH:SHA_256
    $<$<CONFIG:Release>:/Zi>
)
target_link_options(steamfix PRIVATE
    /guard:cf /guard:ehcont /CETCOMPAT /DYNAMICBASE /HIGHENTROPYVA /NXCOMPAT
    $<$<CONFIG:Release>:/DEBUG:FULL>
    $<$<CONFIG:Release>:/Brepro>
    $<$<CONFIG:Release>:/PDBALTPATH:steamfix.pdb>
)
target_link_libraries(steamfix PRIVATE ${STEAMFIX_LIBRARIES})
set_target_properties(steamfix PROPERTIES INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
steamfix_enable_code_analysis(steamfix)

include(CTest)
option(STEAMFIX_ENABLE_COVERAGE "Build the complete Debug coverage source inventory" OFF)
if(STEAMFIX_ENABLE_COVERAGE AND NOT BUILD_TESTING)
    message(FATAL_ERROR "STEAMFIX_ENABLE_COVERAGE requires BUILD_TESTING.")
endif()

function(steamfix_add_test target)
    cmake_parse_arguments(TEST "" "" "ARGS" ${ARGN})
    # Repair code imports TaskDialogIndirect, which requires Common Controls 6
    # even when a test never opens a dialog. Match the application's manifest.
    add_executable(${target} ${TEST_UNPARSED_ARGUMENTS} src/steamfix.manifest)
    target_include_directories(${target} PRIVATE include)
    target_compile_features(${target} PRIVATE cxx_std_20)
    target_compile_definitions(${target} PRIVATE
        UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX STEAMFIX_VERSION="${PROJECT_VERSION}")
    target_compile_options(${target} PRIVATE /W4 /WX /permissive- /EHsc /utf-8 /sdl)
    target_link_libraries(${target} PRIVATE ${STEAMFIX_LIBRARIES})
    steamfix_enable_code_analysis(${target})
    add_test(NAME ${target} COMMAND ${target} ${TEST_ARGS})
    set_tests_properties(${target} PROPERTIES TIMEOUT 120)
endfunction()

if(BUILD_TESTING)
    steamfix_add_test(command_line_tests tests/command_line_tests.cpp src/command_line.cpp)
    steamfix_add_test(win32_tests tests/win32_tests.cpp)

    set(STEAMFIX_APP_TEST_SOURCES ${STEAMFIX_SOURCES})
    list(REMOVE_ITEM STEAMFIX_APP_TEST_SOURCES src/acl.cpp src/app.cpp src/ui.cpp)
    steamfix_add_test(app_tests tests/app_tests.cpp ${STEAMFIX_APP_TEST_SOURCES} ARGS --entry-check)
    steamfix_add_test(acl_tests tests/acl_tests.cpp
        src/elevation.cpp src/logging.cpp src/paths.cpp src/command_line.cpp)
    steamfix_add_test(elevation_tests tests/elevation_tests.cpp src/command_line.cpp src/paths.cpp)
    steamfix_add_test(runtime_tests tests/runtime_tests.cpp
        src/command_line.cpp src/elevation.cpp src/logging.cpp src/ui.cpp)
    steamfix_add_test(process_fault_tests tests/process_fault_tests.cpp
        src/elevation.cpp src/logging.cpp src/paths.cpp src/command_line.cpp)
    steamfix_add_test(repair_workflow_tests tests/repair_workflow_tests.cpp
        src/command_line.cpp src/elevation.cpp src/logging.cpp src/paths.cpp src/process.cpp src/steam.cpp)
    steamfix_add_test(ui_entry_tests tests/ui_entry_tests.cpp src/ui.cpp src/command_line.cpp
        ARGS $<TARGET_FILE:steamfix>)
    add_dependencies(ui_entry_tests steamfix)
    set_tests_properties(ui_entry_tests PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60)
    add_library(overlay_fixture SHARED tests/overlay_fixture.cpp)
    set_target_properties(overlay_fixture PROPERTIES OUTPUT_NAME overlay_fixture PREFIX "")
    target_compile_features(overlay_fixture PRIVATE cxx_std_20)
    target_compile_options(overlay_fixture PRIVATE /W4 /WX /permissive- /EHsc /utf-8 /sdl)
    add_dependencies(runtime_tests overlay_fixture)
    add_dependencies(app_tests overlay_fixture)

    if(STEAMFIX_ENABLE_COVERAGE)
        # Load every production translation unit in one Debug module. This keeps
        # wholly uncalled code in the coverage denominator; it is not a test.
        add_executable(steamfix_coverage_inventory
            cmake/coverage_inventory.cpp src/main.cpp ${STEAMFIX_SOURCES} src/steamfix.manifest)
        target_include_directories(steamfix_coverage_inventory PRIVATE include)
        target_compile_features(steamfix_coverage_inventory PRIVATE cxx_std_20)
        target_compile_definitions(steamfix_coverage_inventory PRIVATE
            UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX STEAMFIX_VERSION="${PROJECT_VERSION}")
        target_compile_options(steamfix_coverage_inventory PRIVATE
            /W4 /WX /permissive- /EHsc /utf-8 /sdl /Zi /Od /Ob0)
        target_link_options(steamfix_coverage_inventory PRIVATE
            /DEBUG:FULL /OPT:NOREF /OPT:NOICF /INCREMENTAL:NO)
        target_link_libraries(steamfix_coverage_inventory PRIVATE ${STEAMFIX_LIBRARIES})
    endif()
endif()

install(TARGETS steamfix RUNTIME DESTINATION .)
install(FILES LICENSE.md README.md DESTINATION .)
install(FILES docs/EXIT_CODES.md DESTINATION docs)
