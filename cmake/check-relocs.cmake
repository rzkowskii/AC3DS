# Fails the build when the ELF holds relocations a .3dsx cannot carry.
# Usage: cmake -DREADELF=<arm-none-eabi-readelf> -DELF=<file> -P check-relocs.cmake
execute_process(COMMAND "${READELF}" -r -W "${ELF}" OUTPUT_VARIABLE relocs RESULT_VARIABLE status ERROR_VARIABLE errors)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "readelf failed on ${ELF}: ${errors}")
endif()
string(REGEX MATCHALL "R_ARM_(BASE_PREL|GOT_BREL|GOT_PREL|GOTOFF32|GOT32)" bad "${relocs}")
list(LENGTH bad count)
if(count GREATER 0)
    list(REMOVE_DUPLICATES bad)
    message(FATAL_ERROR "${ELF} has ${count} GOT-relative relocations (${bad}). "
        "3dsxtool cannot relocate them: build without -fPIC/-fPIE.")
endif()
