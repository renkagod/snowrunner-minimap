# Copies the built DLL and PDB into the game folder: cmake -DSRC_DIR=... -DDEPLOY_DIR=... -P deploy.cmake
# A running game keeps version.dll locked, but a loaded DLL can still be renamed, so a locked copy
# is moved aside and the new one is picked up on the next launch.

foreach(name version.dll version.pdb)
    set(src "${SRC_DIR}/${name}")
    set(dst "${DEPLOY_DIR}/${name}")
    file(COPY_FILE "${src}" "${dst}" RESULT copied)
    if(NOT copied EQUAL 0)
        string(TIMESTAMP stamp "%Y%m%d%H%M%S")
        file(RENAME "${dst}" "${DEPLOY_DIR}/${name}.old-${stamp}" RESULT renamed)
        if(NOT renamed EQUAL 0)
            message(FATAL_ERROR "Cannot replace ${dst}: ${copied}")
        endif()
        file(COPY_FILE "${src}" "${dst}")
        message(STATUS "${name} was in use; old copy moved aside, restart the game to load the new one")
    endif()
endforeach()

# Leftovers from earlier swaps; the ones still loaded by a running game stay until it exits.
file(GLOB leftovers "${DEPLOY_DIR}/version.*.old-*" "${DEPLOY_DIR}/version.old.*")
foreach(old ${leftovers})
    execute_process(COMMAND ${CMAKE_COMMAND} -E rm -f "${old}" RESULT_VARIABLE ignored ERROR_QUIET)
endforeach()
