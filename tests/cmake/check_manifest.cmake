# Verifies the user's original game files against a committed SHA-256 manifest.
# Hashes (not the files themselves) are committed so we can confirm everyone
# works from the same game version. Exit code 77 = skipped (no data present).
# Inputs: DATA_DIR, MANIFEST

if(NOT EXISTS "${DATA_DIR}/GFX")
  message("SKIP: original game data not found at ${DATA_DIR}")
  cmake_language(EXIT 77)
endif()

file(STRINGS "${MANIFEST}" lines)
set(failures 0)
set(checked 0)
foreach(line IN LISTS lines)
  if(line MATCHES "^#" OR line STREQUAL "")
    continue()
  endif()
  if(NOT line MATCHES "^([0-9a-f]+)  (.+)$")
    message(FATAL_ERROR "bad manifest line: ${line}")
  endif()
  set(expected "${CMAKE_MATCH_1}")
  set(rel "${CMAKE_MATCH_2}")
  if(NOT EXISTS "${DATA_DIR}/${rel}")
    # The CD image is optional: CD music can also come from a MUSIC folder of
    # ripped tracks, or the MIDI music is used. Checked only when present.
    if(rel MATCHES "^SCREAM\\.(bin|cue)$")
      message("absent   ${rel} (optional CD image)")
      continue()
    endif()
    message("MISSING  ${rel}")
    math(EXPR failures "${failures} + 1")
    continue()
  endif()
  file(SHA256 "${DATA_DIR}/${rel}" actual)
  math(EXPR checked "${checked} + 1")
  if(NOT actual STREQUAL expected)
    message("MISMATCH ${rel}")
    math(EXPR failures "${failures} + 1")
  endif()
endforeach()

message("checked ${checked} files, ${failures} problems")
if(failures GREATER 0)
  message(FATAL_ERROR "game data does not match manifest")
endif()
