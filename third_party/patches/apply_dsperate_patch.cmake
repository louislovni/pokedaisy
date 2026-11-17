# Our changes to DSperate (third_party/dsperate-lr/DSperate) live in dsperate-capsule.patch: EXPAND's hooks in Gpu3D
# (every polygon past the backface cull, the lists' swap, the latch at VBlank, the box test over the expanded view),
# the same port as melonds-pokedaisy.patch's, and the AES unit's hardware check kept to Linux (getauxval), so the core
# also builds for Apple platforms. Applied once at configure time, like apply_melonds_patch.cmake.
#   include(.../apply_dsperate_patch.cmake) after setting DS
set(PK_DSPERATE_PATCH "${CMAKE_CURRENT_LIST_DIR}/dsperate-capsule.patch")
execute_process(COMMAND git apply --reverse --check "${PK_DSPERATE_PATCH}"
    WORKING_DIRECTORY "${DS}" RESULT_VARIABLE PK_DSPERATE_PATCHED OUTPUT_QUIET ERROR_QUIET)
if(NOT PK_DSPERATE_PATCHED EQUAL 0)
    execute_process(COMMAND git apply "${PK_DSPERATE_PATCH}"
        WORKING_DIRECTORY "${DS}" RESULT_VARIABLE PK_DSPERATE_APPLY ERROR_VARIABLE PK_DSPERATE_ERR)
    if(NOT PK_DSPERATE_APPLY EQUAL 0)
        message(FATAL_ERROR "DSperate patch didn't apply (${PK_DSPERATE_PATCH}): ${PK_DSPERATE_ERR}")
    endif()
    message(STATUS "DSperate: applied dsperate-capsule.patch")
endif()
