# New Nintendo 3DS build of the P3P3DS runner (.3dsx).
#
# Configure with devkitPro's toolchain file, from the repository root:
#   cmake -S platform/3ds -B build/3ds -G Ninja \
#         -DCMAKE_TOOLCHAIN_FILE=<devkitPro>/cmake/3DS.cmake -DCMAKE_BUILD_TYPE=Release
#   cmake --build build/3ds
# The host tools (psp_recomp) come from the host build (P3P_HOST_BUILD,
# default build/), and the decrypted EBOOT from <P3P_PROFILE_DIR>/game/eboot.elf;
# both stay local. The EBOOT is embedded in the .3dsx romfs because the AOT
# code is generated from exactly that file: the resulting .3dsx contains game
# code and is for personal testing only, never for distribution.
cmake_minimum_required(VERSION 3.20)
project(p3p3ds_3ds C CXX)
include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/ccache.cmake")

if(NOT NINTENDO_3DS)
    message(FATAL_ERROR "Configure with -DCMAKE_TOOLCHAIN_FILE=<devkitPro>/cmake/3DS.cmake")
endif()

get_filename_component(P3P_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(P3P_HOST_BUILD "${P3P_ROOT}/build" CACHE PATH "Host build directory providing psp_recomp")
# Game profile: every game-specific path, address and layout comes from here.
set(P3P_PROFILE_DIR "${P3P_ROOT}/profiles/p1p" CACHE PATH "Game profile directory (config/, game/)")
get_filename_component(P3P_PROFILE_DIR "${P3P_PROFILE_DIR}" ABSOLUTE BASE_DIR "${P3P_ROOT}")
include("${P3P_PROFILE_DIR}/config/profile.cmake")
set(P3P_ELF "${P3P_PROFILE_DIR}/game/eboot.elf" CACHE FILEPATH "Decrypted EBOOT of the profile's game")
# Same flags as the P3P3DS Builder (platform/pc/builder/pipeline.cpp). P3P3DS: -O2 since 2026-10-14: in
# Persona's first-person dungeon the game code took about 20 % less main-core time per draw than with
# "-Os -fno-gcse -fno-schedule-insns -fno-schedule-insns2" (picked for size and compile time, whose run-time
# speed was never measured), for 4.2 MiB less heap (docs/CURRENT_STATE.md 2026-10-14). The old flags stay a
# -DP3P_AOT_OPT=... away for a faster build.
set(P3P_AOT_OPT "-O2" CACHE STRING "Optimization flags for the generated AOT units")
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
# Without the local EBOOT (CI, a fresh clone) only the game-independent part is
# built: the P3P3DS runtime objects and libraries the P3P3DS Builder ships, the
# shader and the CWCheat patch list. With it, also the AOT code and the .3dsx.
if(EXISTS "${P3P_ELF}")
    set(P3P_HAVE_GAME ON)
    find_program(P3P_PSP_RECOMP NAMES psp_recomp psp_recomp.exe HINTS "${P3P_HOST_BUILD}/recomp/PSPRecomp" NO_CMAKE_FIND_ROOT_PATH REQUIRED)
else()
    set(P3P_HAVE_GAME OFF)
    message(STATUS "No ${P3P_ELF}: building the runtime without the game (no AOT code, no .3dsx)")
endif()
include("${P3P_PROFILE_DIR}/config/aot_layout.cmake")
if(P3P_HAVE_GAME AND NOT P3P_AOT_UNIT_COUNT)
    message(FATAL_ERROR "P3P_AOT_UNIT_COUNT is not set in ${P3P_PROFILE_DIR}/config/aot_layout.cmake. "
        "Measure it with: ${P3P_PSP_RECOMP} ${P3P_ELF} --auto .tmp/auto ${P3P_AOT_LOAD_BASE} ${P3P_AOT_UNIT_SPAN} "
        "(it prints the translation unit count), set it, then configure again.")
endif()
include("${P3P_ROOT}/cmake/p3p3ds_sources.cmake")

# Code size is the constraint on 3DS (docs/3DS_PLATFORM.md section 8.5): compile
# out chain counters and frontier diagnostics, keep cross-unit chains out of
# line. These change Runtime's layout, so they apply to every target.
add_compile_definitions(PSPRECOMP_AOT_PRODUCTION_FASTPATHS=1 PSPRECOMP_NO_FRONTIER_DIAGNOSTICS=1
                        PSPRECOMP_CHAIN_NOINLINE=1
                        # No PSPRECOMP_WATCH_WRITE debugging (getenv is always empty on 3DS):
                        # guest RAM stores skip the watch flag test.
                        PSPRECOMP_AOT_ASSUME_NO_WRITE_WATCH=1)

# ---- AOT generation (host tool), without per-transfer diagnostics ----------
set(GEN_DIR "${CMAKE_BINARY_DIR}/generated/auto")
set(PATCH_LIST "${CMAKE_BINARY_DIR}/generated/patches.txt")
file(MAKE_DIRECTORY "${GEN_DIR}")
# CWCheat patch list (community codes selected in the profile's config): input
# of the AOT generation and shipped with the P3P3DS Builder. Optional: without
# a profile .ini the list is empty ("none").
set(CWCHEAT_SELECTION "${P3P_PROFILE_DIR}/config/cwcheat_patches.txt")
if(P3P_PROFILE_CWCHEAT_INI)
    set(CWCHEAT_INI "${P3P_PROFILE_CWCHEAT_INI}")
    set(CWCHEAT_DEPENDS "${CWCHEAT_INI}" "${CWCHEAT_SELECTION}")
else()
    set(CWCHEAT_INI "none")
    set(CWCHEAT_DEPENDS "${CWCHEAT_SELECTION}")
endif()
add_custom_command(
    OUTPUT "${PATCH_LIST}"
    COMMAND "${Python3_EXECUTABLE}" "${P3P_ROOT}/tools/cwcheat_patches.py" "${CWCHEAT_INI}"
            "${CWCHEAT_SELECTION}" "${PATCH_LIST}"
    DEPENDS "${P3P_ROOT}/tools/cwcheat_patches.py" ${CWCHEAT_DEPENDS}
    COMMENT "CWCheat patch list"
    VERBATIM
)
add_custom_target(p3p_patch_list ALL DEPENDS "${PATCH_LIST}")
set(AOT_SOURCES "${GEN_DIR}/generated_registry.cpp")
if(P3P_HAVE_GAME)
math(EXPR _last "${P3P_AOT_UNIT_COUNT} - 1")
foreach(_unit RANGE 0 ${_last})
    string(LENGTH "${_unit}" _len)
    math(EXPR _pad "4 - ${_len}")
    string(REPEAT "0" ${_pad} _zeros)
    list(APPEND AOT_SOURCES "${GEN_DIR}/generated_unit_${_zeros}${_unit}.cpp")
endforeach()
# P3P3DS: host routines for guest leaf functions (${P3P_PROFILE_DIR}/config/aot_layout.cmake).
set(P3P_3DS_RECOMP_FLAGS)
foreach(_leaf IN LISTS P3P_AOT_NATIVE_LEAVES)
    list(APPEND P3P_3DS_RECOMP_FLAGS --native-leaf "${_leaf}")
endforeach()
# P3P3DS: guest registers in C++ locals inside each generated block (psp_recomp --gpr-cache).
option(P3P_AOT_GPR_CACHE "Keep guest registers in locals within each generated block" ON)
if(P3P_AOT_GPR_CACHE)
    list(APPEND P3P_3DS_RECOMP_FLAGS --gpr-cache)
endif()
add_custom_command(
    OUTPUT ${AOT_SOURCES} "${GEN_DIR}/generated_units.hpp"
    COMMAND "${P3P_PSP_RECOMP}" "${P3P_ELF}" --auto "${GEN_DIR}" "${P3P_AOT_LOAD_BASE}" "${P3P_AOT_UNIT_SPAN}" "${PATCH_LIST}" --no-transfer-records ${P3P_3DS_RECOMP_FLAGS}
    COMMAND "${Python3_EXECUTABLE}" "${P3P_ROOT}/tools/verify_aot_layout.py" "${GEN_DIR}" "${P3P_AOT_UNIT_COUNT}" "${P3P_AOT_UNIT_SPAN}"
    DEPENDS "${P3P_PSP_RECOMP}" "${P3P_ELF}" "${P3P_PROFILE_DIR}/config/aot_layout.cmake" "${PATCH_LIST}"
    COMMENT "Generating AOT units for 3DS (${P3P_AOT_UNIT_COUNT} x ${P3P_AOT_UNIT_SPAN} bytes)..."
    VERBATIM
)

endif()

list(TRANSFORM PSPRECOMP_RUNTIME_SOURCES PREPEND "${P3P_ROOT}/")
list(TRANSFORM P3P3DS_CORE_SOURCES PREPEND "${P3P_ROOT}/")

add_library(psprecomp_runtime STATIC ${PSPRECOMP_RUNTIME_SOURCES})
target_include_directories(psprecomp_runtime PUBLIC "${P3P_ROOT}/recomp/PSPRecomp/include")
# Direct dispatch table covers at most 4 MiB of guest code (the EBOOT image);
# entries registered elsewhere are looked up in the hash map.
target_compile_definitions(psprecomp_runtime PRIVATE PSPRECOMP_DIRECT_WINDOW_MAX=0x400000u)

if(P3P_HAVE_GAME)
    add_library(p3p_aot STATIC ${AOT_SOURCES})
    target_include_directories(p3p_aot PRIVATE "${GEN_DIR}")
    target_link_libraries(p3p_aot PUBLIC psprecomp_runtime)
    separate_arguments(_aot_opt NATIVE_COMMAND "${P3P_AOT_OPT}")
    target_compile_options(p3p_aot PRIVATE ${_aot_opt})
    # P3P3DS: measurement build: generated code records the guest block it enters and the runner samples
    # it on another core into sdmc:/persona3ds/aot_profile.txt (platform/3ds/main.cpp, AotSampler).
    option(P3P_AOT_PROFILE "Sample which guest code the game's core runs (aot_profile.txt)" OFF)
    if(P3P_AOT_PROFILE)
        target_compile_definitions(p3p_aot PRIVATE PSPRECOMP_AOT_PROFILE)
    endif()
endif()

add_library(p3p3ds_core STATIC ${P3P3DS_CORE_SOURCES})
target_include_directories(p3p3ds_core PUBLIC "${P3P_ROOT}/core/include" "${P3P_PROFILE_DIR}/config")
target_link_libraries(p3p3ds_core PUBLIC psprecomp_runtime)

execute_process(COMMAND git -C "${P3P_ROOT}" rev-parse --short HEAD
                OUTPUT_VARIABLE P3P_BUILD_ID OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
# P3P3DS: re-run this configure step whenever HEAD moves (checkout, pull, commit), so the report's
# `build` line names the commit that was built and not the one that was configured.
if(EXISTS "${P3P_ROOT}/.git/logs/HEAD")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${P3P_ROOT}/.git/logs/HEAD")
endif()
# GE renderer on the PICA200: vertex shader assembled by picasso and embedded.
ctr_add_shader_library(p3p_ge_shader "${CMAKE_CURRENT_SOURCE_DIR}/shaders/ge.v.pica" OUTPUT "${CMAKE_BINARY_DIR}/ge.shbin")
dkp_add_embedded_binary_library(p3p_ge_shader_bin p3p_ge_shader)

# Runner objects (main.o, gpu_renderer.o): linked here into the .3dsx and
# shipped prebuilt with the P3P3DS Builder (platform/pc/builder/make_release.py).
add_library(p3p3ds_objects OBJECT main.cpp gpu_renderer.cpp)
target_compile_definitions(p3p3ds_objects PRIVATE P3P3DS_BUILD_ID="${P3P_BUILD_ID}")
if(P3P_AOT_PROFILE)
    target_compile_definitions(p3p3ds_objects PRIVATE P3P3DS_AOT_PROFILE) # P3P3DS: AotSampler
endif()
target_link_libraries(p3p3ds_objects PRIVATE p3p3ds_core p3p_ge_shader_bin citro3d)

if(P3P_HAVE_GAME)
    add_executable(p3p3ds $<TARGET_OBJECTS:p3p3ds_objects>)
    target_link_libraries(p3p3ds PRIVATE p3p_aot p3p3ds_core p3p_ge_shader_bin citro3d)

    # romfs: the decrypted EBOOT the AOT code was generated from.
    set(ROMFS_DIR "${CMAKE_BINARY_DIR}/romfs")
    file(MAKE_DIRECTORY "${ROMFS_DIR}")
    configure_file("${P3P_ELF}" "${ROMFS_DIR}/eboot.elf" COPYONLY)

    ctr_generate_smdh(OUTPUT "${CMAKE_BINARY_DIR}/p3p3ds.smdh"
        NAME "${P3P_PROFILE_SMDH_NAME}"
        DESCRIPTION "${P3P_PROFILE_SMDH_DESCRIPTION}"
        AUTHOR "${P3P_PROFILE_SMDH_AUTHOR}")
    ctr_create_3dsx(p3p3ds SMDH "${CMAKE_BINARY_DIR}/p3p3ds.smdh" ROMFS "${ROMFS_DIR}")

    # P3P3DS: optional installable .cia, `make -C build/3ds cia` (not part of `all`): bannertool, makerom,
    # banner.png, audio.wav and icon.png (optional) come from P3P_BANNER_DIR, a folder each user fills
    # (not in git). platform/3ds/cia/make_cia.cmake checks them and says what is missing.
    set(P3P_BANNER_DIR "${P3P_ROOT}/banner" CACHE PATH "bannertool, makerom, banner.png, audio.wav, icon.png for the cia target")
    configure_file("${CMAKE_CURRENT_SOURCE_DIR}/cia/p3p3ds.rsf.in" "${CMAKE_BINARY_DIR}/cia/p3p3ds.rsf" @ONLY)
    add_custom_target(cia
        COMMAND "${CMAKE_COMMAND}" "-DBUILD_DIR=${CMAKE_BINARY_DIR}" "-DBANNER_DIR=${P3P_BANNER_DIR}"
                "-DELF_NAME=$<TARGET_FILE_NAME:p3p3ds>" "-DSMDH_NAME=p3p3ds.smdh"
                "-DNAME=${P3P_PROFILE_SMDH_NAME}" "-DDESCRIPTION=${P3P_PROFILE_SMDH_DESCRIPTION}"
                "-DAUTHOR=${P3P_PROFILE_SMDH_AUTHOR}" -P "${CMAKE_CURRENT_SOURCE_DIR}/cia/make_cia.cmake"
        DEPENDS p3p3ds
        COMMENT "Building p3p3ds.cia"
        VERBATIM)
endif()
