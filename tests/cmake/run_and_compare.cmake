# Runs the game executable headless, then compares its output image with a golden image.
# Inputs: EXE, IMGCMP, ARGS (|-separated, OUTPUT path is appended), OUTPUT, GOLDEN, TOLERANCE
# Set environment variable SCREAMER_UPDATE_GOLDEN=1 to overwrite the golden image instead.

string(REPLACE "|" ";" ARG_LIST "${ARGS}")
get_filename_component(out_dir "${OUTPUT}" DIRECTORY)
file(MAKE_DIRECTORY "${out_dir}")
file(REMOVE "${OUTPUT}")

execute_process(COMMAND "${EXE}" ${ARG_LIST} "${OUTPUT}"
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "screamer exited with ${rc}\n${out}\n${err}")
endif()
if(NOT EXISTS "${OUTPUT}")
  message(FATAL_ERROR "screamer did not write ${OUTPUT}\n${out}\n${err}")
endif()

if("$ENV{SCREAMER_UPDATE_GOLDEN}")
  file(COPY_FILE "${OUTPUT}" "${GOLDEN}")
  message(STATUS "updated golden ${GOLDEN}")
  return()
endif()

if(NOT EXISTS "${GOLDEN}")
  message(FATAL_ERROR "golden image missing: ${GOLDEN}\nRun tools/update-golden.ps1 to create it (output is at ${OUTPUT}).")
endif()

string(REGEX REPLACE "\\.png$" ".diff.png" diff "${OUTPUT}")
execute_process(COMMAND "${IMGCMP}" "${GOLDEN}" "${OUTPUT}" --tolerance ${TOLERANCE} --diff "${diff}"
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
message("${out}")
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "image differs from golden ${GOLDEN}\nactual: ${OUTPUT}\ndiff:   ${diff}\n${err}")
endif()
