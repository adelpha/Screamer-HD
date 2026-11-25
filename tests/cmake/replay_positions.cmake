# Plays an original replay in our game headless (--positions) and compares every frame with a
# recording of the original playing it (tools/dosbox/record-replay.ps1).
# Inputs: EXE, TRACECMP, DATA_DIR, RPL, TRACK, TRACE, OFFSET, FRAMES, OUTPUT
file(REMOVE "${OUTPUT}")
execute_process(COMMAND "${EXE}" --headless --mute --data "${DATA_DIR}" --track ${TRACK} --replay "${RPL}"
                        --frames ${FRAMES} --positions "${OUTPUT}"
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "screamer exited with ${rc}\n${out}\n${err}")
endif()
execute_process(COMMAND "${TRACECMP}" "${TRACE}" --positions "${OUTPUT}" ${OFFSET}
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
message(STATUS "${out}")
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "the replay differs from the original\n${out}\n${err}")
endif()
