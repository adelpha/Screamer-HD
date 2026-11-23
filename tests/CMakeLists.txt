set(GOLDEN_DIR ${CMAKE_CURRENT_SOURCE_DIR}/golden)
set(TEST_OUT_DIR ${CMAKE_BINARY_DIR}/test-output)

# --- Unit tests: one executable, self-registering cases ---
add_executable(unit_tests
  unit/test_main.cpp
  unit/test_framebuffer.cpp
  unit/test_image.cpp
  unit/test_gamedata.cpp
)
target_include_directories(unit_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(unit_tests PRIVATE screamer_core)
add_test(NAME unit COMMAND unit_tests)
set_tests_properties(unit PROPERTIES LABELS "unit")

# --- Headless render tests: run the real executable offscreen, compare with golden images ---
# Golden images are our own output (not game data) and are committed. Regenerate
# them deliberately with tools/update-golden.ps1 when rendering changes on purpose.
function(add_render_test name)
  cmake_parse_arguments(RT "" "GOLDEN;OUTPUT;TOLERANCE" "ARGS" ${ARGN})
  string(REPLACE ";" "|" RT_ARGS_PIPE "${RT_ARGS}")
  if(NOT RT_TOLERANCE)
    set(RT_TOLERANCE 0)
  endif()
  add_test(NAME render.${name}
    COMMAND ${CMAKE_COMMAND}
      -DEXE=$<TARGET_FILE:screamer>
      -DIMGCMP=$<TARGET_FILE:imgcmp>
      "-DARGS=${RT_ARGS_PIPE}"
      -DOUTPUT=${TEST_OUT_DIR}/${RT_OUTPUT}
      -DGOLDEN=${GOLDEN_DIR}/${RT_GOLDEN}
      -DTOLERANCE=${RT_TOLERANCE}
      -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/run_and_compare.cmake)
  set_tests_properties(render.${name} PROPERTIES LABELS "render")
endfunction()

add_render_test(testpattern_fb_f0  ARGS --headless --frames 1  --dump-fb    OUTPUT testpattern_fb_f0.png  GOLDEN testpattern_fb_f0.png)
add_render_test(testpattern_fb_f60 ARGS --headless --frames 61 --dump-fb    OUTPUT testpattern_fb_f60.png GOLDEN testpattern_fb_f60.png)
add_render_test(testpattern_window_640x480
  ARGS --headless --frames 1 --size 640x480 --screenshot
  OUTPUT testpattern_window_640x480.png GOLDEN testpattern_window_640x480.png)
add_render_test(testpattern_window_widescreen
  ARGS --headless --frames 1 --size 1280x720 --screenshot
  OUTPUT testpattern_window_1280x720.png GOLDEN testpattern_window_1280x720.png)

# --- Smoke test: the executable starts, runs and exits cleanly ---
add_test(NAME smoke.headless_run COMMAND screamer --headless --frames 10)
set_tests_properties(smoke.headless_run PROPERTIES LABELS "smoke" PASS_REGULAR_EXPRESSION "frames=10")

# --- Original game data: verify the user's copy against the recorded manifest ---
add_test(NAME gamedata.manifest
  COMMAND ${CMAKE_COMMAND}
    -DDATA_DIR=${SCREAMER_DATA_DIR}
    -DMANIFEST=${CMAKE_SOURCE_DIR}/reference/manifest/screamer_files.sha256
    -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_manifest.cmake)
set_tests_properties(gamedata.manifest PROPERTIES LABELS "gamedata" SKIP_RETURN_CODE 77)
