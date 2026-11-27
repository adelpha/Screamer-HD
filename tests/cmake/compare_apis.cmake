# Runs the game twice with the real video driver on a hidden window (--hidden), once through
# SDL's renderer and once through our OpenGL context, and compares the two screenshots.
# Inputs: EXE, IMGCMP, ARGS (|-separated), OUTPUT (prefix of the two PNGs), TOLERANCE,
# MAX_FRACTION (share of pixels that may differ by more than TOLERANCE), EXPECT (regex the
# OpenGL run's log must also match; optional).
# Exits with 77 (skipped) where there is no OpenGL 3.3.

string(REPLACE "|" ";" ARG_LIST "${ARGS}")
get_filename_component(out_dir "${OUTPUT}" DIRECTORY)
file(MAKE_DIRECTORY "${out_dir}")
if(NOT TOLERANCE)
  set(TOLERANCE 0)
endif()
if(NOT MAX_FRACTION)
  set(MAX_FRACTION 0)
endif()

foreach(api sdl opengl)
  set(shot "${OUTPUT}_${api}.png")
  file(REMOVE "${shot}")
  execute_process(COMMAND "${EXE}" --hidden --video-api ${api} ${ARG_LIST} --screenshot "${shot}"
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "screamer (${api}) exited with ${rc}\n${out}\n${err}")
  endif()
  if(api STREQUAL "opengl")
    if(NOT "${out}${err}" MATCHES "renderer: opengl")
      message("no OpenGL 3.3 here: skipped\n${out}${err}")
      cmake_language(EXIT 77)
    endif()
    if(EXPECT AND NOT "${out}${err}" MATCHES "${EXPECT}")
      message(FATAL_ERROR "the OpenGL run's log does not match ${EXPECT}\n${out}\n${err}")
    endif()
  endif()
  if(NOT EXISTS "${shot}")
    message(FATAL_ERROR "screamer (${api}) did not write ${shot}\n${out}\n${err}")
  endif()
endforeach()

execute_process(COMMAND "${IMGCMP}" "${OUTPUT}_sdl.png" "${OUTPUT}_opengl.png" --tolerance ${TOLERANCE}
    --max-diff-fraction ${MAX_FRACTION} --diff "${OUTPUT}_diff.png"
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
message("${out}")
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "the OpenGL picture differs from SDL's: ${OUTPUT}_diff.png\n${err}")
endif()
