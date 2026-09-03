cmake_minimum_required(VERSION 3.20)

project(WaveRace64Recomp
    DESCRIPTION "Native PC port of Wave Race 64 via static recompilation"
    LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

# ---------------------------------------------------------------------------
#  Phase gating
#
#  This project is built in phases (see docs/PLAN.md). Each option below is
#  turned on by the phase that earns it, so a partially finished tree always
#  configures and builds.
# ---------------------------------------------------------------------------
option(WR64_WITH_RUNTIME    "Link ultramodern + librecomp + RT64 (phase 03)" OFF)
option(WR64_WITH_RECOMPILED "Compile generated code in RecompiledFuncs/ (phase 02)" OFF)
option(WR64_BUILD_RECOMPILER "Also build the N64Recomp tool from lib/ (phase 02)" OFF)

# ---------------------------------------------------------------------------
#  Compiler policy
#
#  N64Recomp's output is validated against Clang and MSVC. Modern GCC's
#  optimizer has produced miscompiles of recompiled code, so warn loudly
#  rather than silently building something that fails at runtime only.
# ---------------------------------------------------------------------------
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    message(WARNING
        "Building with GCC. N64Recomp output is known to be sensitive to modern "
        "GCC optimizations; Clang or MSVC is strongly recommended.")
endif()

if(MSVC)
    add_compile_options(/utf-8)
endif()

# ---------------------------------------------------------------------------
#  Dependencies
# ---------------------------------------------------------------------------
set(WR64_LIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/lib")

if(WR64_WITH_RUNTIME)
    if(NOT EXISTS "${WR64_LIB_DIR}/RT64/CMakeLists.txt")
        message(FATAL_ERROR
            "Submodules are missing. Run: git submodule update --init --recursive")
    endif()

    set(RT64_STATIC ON CACHE BOOL "" FORCE)
    add_subdirectory("${WR64_LIB_DIR}/RT64" EXCLUDE_FROM_ALL)
    add_subdirectory("${WR64_LIB_DIR}/N64ModernRuntime" EXCLUDE_FROM_ALL)
endif()

# The recompiler tool lives inside N64ModernRuntime as its own submodule. Using
# that copy rather than a separately pinned one keeps the generated code and the
# runtime that consumes it on the same revision of the shared headers.
if(WR64_BUILD_RECOMPILER AND NOT TARGET N64Recomp)
    add_subdirectory("${WR64_LIB_DIR}/N64ModernRuntime/N64Recomp" EXCLUDE_FROM_ALL)
endif()

# ---------------------------------------------------------------------------
#  Recompiled game code (generated from the user's own ROM; never committed)
# ---------------------------------------------------------------------------
if(WR64_WITH_RECOMPILED)
    file(GLOB WR64_RECOMPILED_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/RecompiledFuncs/*.c")

    if(NOT WR64_RECOMPILED_SOURCES)
        message(FATAL_ERROR
            "WR64_WITH_RECOMPILED is ON but RecompiledFuncs/ is empty.\n"
            "Generate it first: N64Recomp recomp/wr64.toml")
    endif()

    add_library(wr64_recompiled STATIC ${WR64_RECOMPILED_SOURCES})
    target_include_directories(wr64_recompiled PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        # recomp.h and the librecomp headers the generated code includes, plus
        # RecompiledFuncs itself for funcs.h and reimplemented_decls.h.
        "${WR64_LIB_DIR}/N64ModernRuntime/N64Recomp/include"
        "${WR64_LIB_DIR}/N64ModernRuntime/librecomp/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/RecompiledFuncs")

    # Recompiled code is machine-generated: its warnings are noise, and it must
    # never be built with strict aliasing or signed-overflow assumptions.
    if(NOT MSVC)
        target_compile_options(wr64_recompiled PRIVATE
            -fno-strict-aliasing -fwrapv -w)
    else()
        target_compile_options(wr64_recompiled PRIVATE /w)
    endif()
endif()

# ---------------------------------------------------------------------------
#  The port itself
# ---------------------------------------------------------------------------
add_executable(WaveRace64Recomp
    src/main.cpp
    src/rom.cpp
)

target_include_directories(WaveRace64Recomp PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include")

if(WR64_WITH_RUNTIME)
    target_link_libraries(WaveRace64Recomp PRIVATE rt64 ultramodern librecomp)
    target_compile_definitions(WaveRace64Recomp PRIVATE WR64_WITH_RUNTIME=1)

    if(WIN32)
        # RT64 vendors SDL2 for Windows and points at it with link_directories(),
        # which is directory-scoped: it applies to targets declared inside RT64's
        # own CMakeLists but not to ours. The library *names* propagate to us
        # through target_link_libraries, so the link asks for SDL2.lib without
        # knowing where it lives. Supply the search path rather than patching the
        # submodule.
        set(WR64_SDL2_DIR
            "${WR64_LIB_DIR}/RT64/src/contrib/mupen64plus-win32-deps/SDL2-2.26.3")
        target_link_directories(WaveRace64Recomp PRIVATE "${WR64_SDL2_DIR}/lib/x64")

        # SDL2 is a DLL here, so it has to sit beside the executable to run.
        add_custom_command(TARGET WaveRace64Recomp POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${WR64_SDL2_DIR}/lib/x64/SDL2.dll"
                    "$<TARGET_FILE_DIR:WaveRace64Recomp>"
            COMMENT "Copying SDL2.dll next to the executable")
    endif()
endif()

if(WR64_WITH_RECOMPILED)
    # The archive-group flags exist to break the cycle between the recompiled
    # code and librecomp, so they are only meaningful when both are in the link.
    # They are also ld/lld-ELF syntax: lld-link ignores them with a warning, so
    # they must not be emitted for an MSVC-style link at all.
    if(WR64_WITH_RUNTIME AND UNIX AND NOT APPLE)
        target_link_libraries(WaveRace64Recomp PRIVATE
            -Wl,--start-group wr64_recompiled librecomp -Wl,--end-group)
    else()
        target_link_libraries(WaveRace64Recomp PRIVATE wr64_recompiled)
    endif()
    target_compile_definitions(WaveRace64Recomp PRIVATE WR64_WITH_RECOMPILED=1)
endif()

message(STATUS "WaveRace64Recomp configuration:")
message(STATUS "  compiler          ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  runtime + RT64    ${WR64_WITH_RUNTIME}")
message(STATUS "  recompiled code   ${WR64_WITH_RECOMPILED}")
message(STATUS "  recompiler tool   ${WR64_BUILD_RECOMPILER}")
