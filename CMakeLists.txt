cmake_minimum_required(VERSION 3.20)

# Select the MSVC runtime via CMAKE_MSVC_RUNTIME_LIBRARY (requires CMP0091 NEW).
if (POLICY CMP0091)
    cmake_policy(SET CMP0091 NEW)
endif()

# Pick up the vcpkg manifest automatically once we add one (e.g. for MinHook).
# Until src/vcpkg.json exists this is a no-op and the project builds with no
# third-party dependencies. scripts/build_windows.ps1 mirrors this: it only
# wires the vcpkg toolchain when src/vcpkg.json is present.
if (NOT DEFINED VCPKG_MANIFEST_DIR AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/vcpkg.json")
    set(VCPKG_MANIFEST_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src" CACHE PATH "vcpkg manifest directory")
endif()

project(mstsfence LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Compile the rdpedisp DPI-investigation scaffolding (observe hooks + DPI API probes) into
# the hook DLL. OFF by default -- the DPI override itself is always built; this just
# adds the verbose logging that found it. Enable with -DMSTSFENCE_DPI_DIAG=ON.
option(MSTSFENCE_DPI_DIAG "Build the rdpedisp DPI diagnostics scaffolding" OFF)

# ---------------------------------------------------------------------------
# Version (W.X.Y.Z)
#
# Passed as -DMSTSFENCE_VERSION=... by scripts/build_windows.ps1; defaults so a
# plain `cmake ...` invocation still configures without a version flag.
# ---------------------------------------------------------------------------
if (NOT DEFINED MSTSFENCE_VERSION OR MSTSFENCE_VERSION STREQUAL "")
    set(MSTSFENCE_VERSION "0.1.1.2")
endif()

string(REPLACE "." ";" _mstsfence_version_parts "${MSTSFENCE_VERSION}")
list(LENGTH _mstsfence_version_parts _mstsfence_version_count)
if (NOT _mstsfence_version_count EQUAL 4)
    message(FATAL_ERROR
        "MSTSFENCE_VERSION must be W.X.Y.Z (4 dot-separated integers); got '${MSTSFENCE_VERSION}'")
endif()
foreach (_part IN LISTS _mstsfence_version_parts)
    if (NOT _part MATCHES "^[0-9]+$")
        message(FATAL_ERROR
            "MSTSFENCE_VERSION components must be non-negative integers; got '${_part}'")
    endif()
endforeach()
list(GET _mstsfence_version_parts 0 MSTSFENCE_VERSION_MAJOR)
list(GET _mstsfence_version_parts 1 MSTSFENCE_VERSION_MINOR)
list(GET _mstsfence_version_parts 2 MSTSFENCE_VERSION_PATCH)
list(GET _mstsfence_version_parts 3 MSTSFENCE_VERSION_BUILD)
message(STATUS "mstsfence version: ${MSTSFENCE_VERSION}")

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/src/version.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/version.h"
    @ONLY)

# ---------------------------------------------------------------------------
# MSVC toolchain settings
# ---------------------------------------------------------------------------
if (MSVC AND WIN32)
    # Static CRT so the shipped binaries need no VC++ redistributable.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

    # Embed debug info in each .obj (/Z7) instead of a shared compile-time PDB
    # (/Zi), which races under parallel Ninja (C1041). The linker still emits the
    # final per-target .pdb. Applied to every config so Release also ships a PDB
    # to symbolicate field crashes.
    foreach (flag_var
            CMAKE_CXX_FLAGS_DEBUG           CMAKE_C_FLAGS_DEBUG
            CMAKE_CXX_FLAGS_RELEASE         CMAKE_C_FLAGS_RELEASE
            CMAKE_CXX_FLAGS_RELWITHDEBINFO  CMAKE_C_FLAGS_RELWITHDEBINFO
            CMAKE_CXX_FLAGS_MINSIZEREL      CMAKE_C_FLAGS_MINSIZEREL)
        if (DEFINED ${flag_var})
            if ("${${flag_var}}" MATCHES "/Zi")
                string(REGEX REPLACE "/Zi" "/Z7" ${flag_var} "${${flag_var}}")
            elseif (NOT "${${flag_var}}" MATCHES "/Z7")
                string(APPEND ${flag_var} " /Z7")
            endif()
            set(${flag_var} "${${flag_var}}" CACHE STRING "" FORCE)
        endif()
    endforeach()

    # Non-Debug configs don't imply /DEBUG, so /Z7 alone would bloat .objs
    # without producing a usable .pdb. Add /DEBUG and re-assert /OPT:REF,ICF
    # (which /DEBUG otherwise turns off, retaining dead code).
    foreach (linker_flag_var
            CMAKE_EXE_LINKER_FLAGS_RELEASE
            CMAKE_EXE_LINKER_FLAGS_RELWITHDEBINFO
            CMAKE_EXE_LINKER_FLAGS_MINSIZEREL
            CMAKE_SHARED_LINKER_FLAGS_RELEASE
            CMAKE_SHARED_LINKER_FLAGS_RELWITHDEBINFO
            CMAKE_SHARED_LINKER_FLAGS_MINSIZEREL)
        if (DEFINED ${linker_flag_var} AND NOT "${${linker_flag_var}}" MATCHES "/DEBUG")
            string(APPEND ${linker_flag_var} " /DEBUG /OPT:REF /OPT:ICF")
            set(${linker_flag_var} "${${linker_flag_var}}" CACHE STRING "" FORCE)
        endif()
    endforeach()
endif()

# ---------------------------------------------------------------------------
# Dependencies -- Microsoft Detours (vcpkg `detours`) for the inline hook.
# ---------------------------------------------------------------------------
# Prefer the vcpkg CMake config (handles debug/release automatically); fall back
# to locating the lib/headers in the vcpkg-installed tree, which always works.
find_package(unofficial-detours CONFIG QUIET)
add_library(detours_imported INTERFACE)
if (TARGET unofficial::detours::detours)
    target_link_libraries(detours_imported INTERFACE unofficial::detours::detours)
    message(STATUS "Detours: vcpkg config package")
else()
    find_path(DETOURS_INCLUDE_DIR NAMES detours/detours.h detours.h REQUIRED)
    find_library(DETOURS_LIBRARY_RELEASE NAMES detours
        PATHS "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib" NO_DEFAULT_PATH)
    find_library(DETOURS_LIBRARY_DEBUG NAMES detours
        PATHS "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/debug/lib" NO_DEFAULT_PATH)
    if (NOT DETOURS_LIBRARY_RELEASE)
        find_library(DETOURS_LIBRARY_RELEASE NAMES detours REQUIRED)
    endif()

    target_include_directories(detours_imported INTERFACE "${DETOURS_INCLUDE_DIR}")
    if (EXISTS "${DETOURS_INCLUDE_DIR}/detours/detours.h")
        # Let sources do #include <detours.h> regardless of the port's layout.
        target_include_directories(detours_imported INTERFACE "${DETOURS_INCLUDE_DIR}/detours")
    endif()
    if (DETOURS_LIBRARY_DEBUG)
        target_link_libraries(detours_imported INTERFACE
            "$<$<CONFIG:Debug>:${DETOURS_LIBRARY_DEBUG}>"
            "$<$<NOT:$<CONFIG:Debug>>:${DETOURS_LIBRARY_RELEASE}>")
    else()
        target_link_libraries(detours_imported INTERFACE "${DETOURS_LIBRARY_RELEASE}")
    endif()
    message(STATUS "Detours: ${DETOURS_LIBRARY_RELEASE}")
endif()

# ---------------------------------------------------------------------------
# Dark-mode theming -- umbra (vcpkg `umbra`, resolved from the WM_UMBRA git
# registry declared in src/vcpkg.json). The hook DLL uses it to dark-style mstsc.
# ---------------------------------------------------------------------------
find_package(umbra CONFIG REQUIRED)

# ---------------------------------------------------------------------------
# Version resources (VERSIONINFO) + app icon, templated per target.
# ---------------------------------------------------------------------------
# Absolute path to the app icon, with backslashes doubled for the .rc. The
# generated .rc lives under build/generated/, so a relative path wouldn't resolve.
file(TO_NATIVE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/resources/fence-icon.ico" _mstsfence_icon_native)
string(REPLACE "\\" "\\\\" MSTSFENCE_ICON_PATH "${_mstsfence_icon_native}")

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/src/mstsfence.rc.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/mstsfence.rc" @ONLY)
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/src/mstsfencehook.rc.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/mstsfencehook.rc" @ONLY)

# The app manifest is templated too, so the assemblyIdentity version tracks
# MSTSFENCE_VERSION (one source of truth for a release) and stays arch-neutral
# (processorArchitecture="*") for both amd64 and arm64. CMake embeds any source
# with a .manifest extension via mt.exe, generated location included.
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/src/mstsfence.exe.manifest.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/mstsfence.exe.manifest" @ONLY)

# ---------------------------------------------------------------------------
# mstsfencehook.dll -- the injected hook. The OS maps it into GUI processes via the
# controller's global WH_CBT hook; it only does anything inside mstsc.exe.
# ---------------------------------------------------------------------------
add_custom_target(
    UnstickPayloadTarget
    COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass -File "${CMAKE_CURRENT_SOURCE_DIR}/scripts/unstick-payload.ps1" -Dll "${CMAKE_CURRENT_BINARY_DIR}/mstsfencehook.dll" || cd .
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "Unsticking mstsfencehook.dll"
)
add_library(mstsfencehook SHARED
    "src/hook.cpp"
    "src/rdpedisp.cpp"
    "src/darkmode.cpp"
    "src/settings.cpp"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/mstsfencehook.rc"
)
target_include_directories(mstsfencehook PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_compile_definitions(mstsfencehook PRIVATE
    WIN32 UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
    _WIN32_WINNT=0x0A00 NTDDI_VERSION=0x0A000000)   # Win10+ for umbra's dark-mode APIs
if (MSTSFENCE_DPI_DIAG)
    target_compile_definitions(mstsfencehook PRIVATE MSTSFENCE_DPI_DIAG=1)
endif()
target_link_libraries(mstsfencehook PRIVATE
    detours_imported umbra::umbra user32 advapi32 uxtheme dwmapi gdi32 comctl32 shcore)
if (MSVC)
    target_compile_options(mstsfencehook PRIVATE /utf-8 /W4)
endif()
add_dependencies(mstsfencehook UnstickPayloadTarget)

# ---------------------------------------------------------------------------
# mstsfence.exe -- the controller: a tray app that installs the global WH_CBT hook,
# survives Explorer restarts, and registers itself to run at login. `--diag` /
# `--help` attach a console. GUI subsystem; wWinMain entry point.
# ---------------------------------------------------------------------------
add_executable(mstsfence WIN32
    "src/main.cpp"
    "src/settings.cpp"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/mstsfence.rc"
    # CMake auto-recognizes .manifest sources and embeds them via mt.exe.
    "${CMAKE_CURRENT_BINARY_DIR}/generated/mstsfence.exe.manifest"
)
target_include_directories(mstsfence PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_compile_definitions(mstsfence PRIVATE
    WIN32 UNICODE _UNICODE _WINDOWS WIN32_LEAN_AND_MEAN NOMINMAX
    _WIN32_WINNT=0x0A00 NTDDI_VERSION=0x0A000000)   # Win10+ for umbra's dark-mode APIs
target_link_libraries(mstsfence PRIVATE
    user32 shell32 advapi32 ole32 umbra::umbra uxtheme dwmapi gdi32 comctl32)
if (MSVC)
    target_compile_options(mstsfence PRIVATE /utf-8 /W4)
    target_link_options(mstsfence PRIVATE "/ENTRY:wWinMainCRTStartup")
endif()

# The controller loads mstsfencehook.dll from its own directory at runtime; with the
# default single-config (Ninja) output dir both land side by side. Order the
# build so the DLL is present next to the exe.
add_dependencies(mstsfence mstsfencehook)
