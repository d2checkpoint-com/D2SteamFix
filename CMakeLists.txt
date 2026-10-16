cmake_minimum_required(VERSION 3.24)

project(D2SteamFix VERSION 1.1.0 LANGUAGES CXX)

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(NOT WIN32 OR NOT MSVC)
    message(FATAL_ERROR "D2SteamFix requires 64-bit Windows and Microsoft Visual C++.")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "D2SteamFix supports only 64-bit Windows.")
endif()

configure_file("${CMAKE_CURRENT_SOURCE_DIR}/src/steamfix.rc.in"
               "${CMAKE_CURRENT_BINARY_DIR}/steamfix.rc" @ONLY)

add_executable(steamfix WIN32
    src/main.cpp
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
    "${CMAKE_CURRENT_BINARY_DIR}/steamfix.rc"
    src/steamfix.manifest
    src/ui.cpp
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
target_link_libraries(steamfix PRIVATE advapi32 comctl32 comdlg32 crypt32 ole32 shell32 user32)
set_target_properties(steamfix PROPERTIES INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)

install(TARGETS steamfix RUNTIME DESTINATION .)
install(FILES LICENSE.md README.md DESTINATION .)
install(FILES docs/EXIT_CODES.md DESTINATION docs)
