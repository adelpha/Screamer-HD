# Runs the game twice on a hidden window (--hidden): the 3D view drawn by the software renderer
# and by the hardware renderer (--renderer3d hardware), and compares the two pictures as drawn
# (--dump-fb: render size, HUD included).
# Inputs: EXE, IMGCMP, ARGS (|-separated), OUTPUT (prefix of the PNGs), TOLERANCE, MAX_FRACTION.
# Exits with 77 (skipped) where there is no OpenGL 3.3 hardware renderer.

string(REPLACE "|" ";" ARG_LIST "${ARGS}")
get_filename_component(out_dir "${OUTPUT}" DIRECTORY)
file(MAKE_DIRECTORY "${out_dir}")

foreach(r software hardware)
  set(shot "${OUTPUT}_${r}.png")
  file(REMOVE "${shot}")
  execute_process(COMMAND "${EXE}" --hidden --mute --video-api opengl --renderer3d ${r} ${ARG_LIST} --dump-fb "${shot}"
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "screamer (${r}) exited with ${rc}\n${out}\n${err}")
  endif()
  if(r STREQUAL "hardware" AND NOT "${out}${err}" MATCHES "hardware renderer:")
    message("no hardware renderer here: skipped\n${out}${err}")
    cmake_language(EXIT 77)
  endif()
endforeach()

execute_process(COMMAND "${IMGCMP}" "${OUTPUT}_software.png" "${OUTPUT}_hardware.png" --tolerance ${TOLERANCE}
    --max-diff-fraction ${MAX_FRACTION} --diff "${OUTPUT}_diff.png"
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
message("${out}")
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "the hardware renderer's picture differs from the software renderer's: ${OUTPUT}_diff.png\n${err}")
endif()
