# P3P3DS: builds p3p3ds.cia from the linked p3p3ds.elf (target `cia` of platform/3ds/CMakeLists.txt):
#   make -C build/3ds cia
# Inputs come from the user's own <repo>/banner folder (not in git, .gitignore):
#   bannertool[.exe], makerom[.exe]   the tools
#   banner.png (256x128), audio.wav   the HOME Menu banner and its sound (bannertool makebanner)
#   icon.png (48x48, optional)        the icon; without it the .3dsx metadata (p3p3ds.smdh) is used
# Runs in the build directory (BUILD_DIR) with relative paths, so the native Windows tools get no MSYS
# paths. Variables (-D): BUILD_DIR, BANNER_DIR, ELF_NAME, SMDH_NAME, NAME, DESCRIPTION, AUTHOR.
cmake_minimum_required(VERSION 3.16)

function(find_tool var name)
    foreach(candidate "${BANNER_DIR}/${name}.exe" "${BANNER_DIR}/${name}")
        if(EXISTS "${candidate}" AND NOT IS_DIRECTORY "${candidate}")
            set(${var} "${candidate}" PARENT_SCOPE)
            return()
        endif()
    endforeach()
    message(FATAL_ERROR "CIA: ${name} not found in ${BANNER_DIR} (put ${name} or ${name}.exe there)")
endfunction()

find_tool(BANNERTOOL bannertool)
find_tool(MAKEROM makerom)
foreach(required banner.png audio.wav)
    if(NOT EXISTS "${BANNER_DIR}/${required}")
        message(FATAL_ERROR "CIA: ${BANNER_DIR}/${required} is missing (banner.png 256x128, audio.wav for the HOME Menu banner)")
    endif()
endforeach()

set(work "${BUILD_DIR}/cia")
file(MAKE_DIRECTORY "${work}")
file(COPY "${BANNER_DIR}/banner.png" "${BANNER_DIR}/audio.wav" DESTINATION "${work}")

function(run)
    execute_process(COMMAND ${ARGN} WORKING_DIRECTORY "${BUILD_DIR}" RESULT_VARIABLE result)
    if(NOT result EQUAL 0)
        list(GET ARGN 0 tool)
        message(FATAL_ERROR "CIA: ${tool} failed (${result})")
    endif()
endfunction()

run("${BANNERTOOL}" makebanner -i cia/banner.png -a cia/audio.wav -o cia/banner.bnr)
if(EXISTS "${BANNER_DIR}/icon.png")
    file(COPY "${BANNER_DIR}/icon.png" DESTINATION "${work}")
    run("${BANNERTOOL}" makesmdh -s "${NAME}" -l "${DESCRIPTION}" -p "${AUTHOR}" -i cia/icon.png -o cia/icon.smdh)
    set(icon cia/icon.smdh)
else()
    if(NOT EXISTS "${BUILD_DIR}/${SMDH_NAME}")
        message(FATAL_ERROR "CIA: no ${BANNER_DIR}/icon.png and no ${BUILD_DIR}/${SMDH_NAME}: add icon.png, or build the .3dsx first (make -C build/3ds)")
    endif()
    message(STATUS "CIA: no ${BANNER_DIR}/icon.png, using ${SMDH_NAME}")
    set(icon "${SMDH_NAME}")
endif()
run("${MAKEROM}" -f cia -o p3p3ds.cia -target t -exefslogo -rsf cia/p3p3ds.rsf -elf "${ELF_NAME}" -icon "${icon}" -banner cia/banner.bnr)
message(STATUS "CIA: ${BUILD_DIR}/p3p3ds.cia")
