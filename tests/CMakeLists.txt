set(GOLDEN_DIR ${CMAKE_CURRENT_SOURCE_DIR}/golden)
set(TEST_OUT_DIR ${CMAKE_BINARY_DIR}/test-output)

# --- Unit tests: one executable, self-registering cases ---
add_executable(unit_tests
  unit/test_main.cpp
  ../src/debug_report.cpp
  ../src/input.cpp
  unit/test_framebuffer.cpp
  unit/test_image.cpp
  unit/test_gamedata.cpp
  unit/test_archive.cpp
  unit/test_dtr.cpp
  unit/test_pcx.cpp
  unit/test_sim_clock.cpp
  unit/test_track.cpp
  unit/test_render_fill.cpp
  unit/test_car.cpp
  unit/test_hud.cpp
  unit/test_sound.cpp
  unit/test_fm_synth.cpp
  unit/test_menu.cpp
  unit/test_replay.cpp
  unit/test_controls.cpp
)
target_include_directories(unit_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/src)
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

add_render_test(testpattern_fb_f0  ARGS --headless --scene testpattern --frames 1  --dump-fb    OUTPUT testpattern_fb_f0.png  GOLDEN testpattern_fb_f0.png)
add_render_test(testpattern_fb_f60 ARGS --headless --scene testpattern --frames 61 --dump-fb    OUTPUT testpattern_fb_f60.png GOLDEN testpattern_fb_f60.png)
add_render_test(testpattern_window_640x480
  ARGS --headless --scene testpattern --frames 1 --size 640x480 --screenshot
  OUTPUT testpattern_window_640x480.png GOLDEN testpattern_window_640x480.png)
add_render_test(testpattern_window_widescreen
  ARGS --headless --scene testpattern --frames 1 --size 1280x720 --screenshot
  OUTPUT testpattern_window_1280x720.png GOLDEN testpattern_window_1280x720.png)

# --- Smoke test: the executable starts, runs and exits cleanly ---
add_test(NAME smoke.headless_run COMMAND screamer --headless --scene testpattern --frames 10)
set_tests_properties(smoke.headless_run PROPERTIES LABELS "smoke" PASS_REGULAR_EXPRESSION "frames=10")

# --- Car physics against recordings of the original (tools/dosbox/record-race.ps1) ---
# The recordings stay in artifacts/traces (git-ignored); a test exists only when its recording
# does. Allowed differences: bumps from CPU cars (not ported yet) and +-1 ground height on slopes.
# Bit for bit (--exact): only frames where a CPU car bumps the player (not in these
# player-only recordings) may differ.
foreach(trace accel:1 steer:0 wall:0 skid:0 fast:2)
  string(REPLACE ":" ";" parts ${trace})
  list(GET parts 0 name)
  list(GET parts 1 allowed)
  set(file ${CMAKE_SOURCE_DIR}/artifacts/traces/${name}.trace)
  if(EXISTS ${file} AND EXISTS ${SCREAMER_DATA_DIR}/GFX/MAP1H.DAT)
    add_test(NAME physics.${name}
      COMMAND tracecmp ${file} ${SCREAMER_DATA_DIR}/GFX/MAP1H.DAT MAP1 --exact --max-diff ${allowed})
    set_tests_properties(physics.${name} PROPERTIES LABELS "physics")
  endif()
endforeach()
# The control setups (trace version 5, with the bindings): a joystick's analog axes, the same
# axes read as keys, a manual gearbox's gear keys. Recorded on MAP2 (the track CHOICE.DAT had).
foreach(name analog digital gears)
  set(file ${CMAKE_SOURCE_DIR}/artifacts/traces/${name}.trace)
  if(EXISTS ${file} AND EXISTS ${SCREAMER_DATA_DIR}/GFX/MAP2H.DAT)
    add_test(NAME physics.${name}
      COMMAND tracecmp ${file} ${SCREAMER_DATA_DIR}/GFX/MAP2H.DAT MAP2 --exact)
    set_tests_properties(physics.${name} PROPERTIES LABELS "physics")
  endif()
endforeach()
# Recordings with the CPU cars (RECORD_OPPONENTS=1): the whole race is stepped.
foreach(name opp_start opp_300 opp_700 tunnel)
  set(file ${CMAKE_SOURCE_DIR}/artifacts/traces/${name}.trace)
  if(EXISTS ${file} AND EXISTS ${SCREAMER_DATA_DIR}/GFX/MAP1H.DAT)
    add_test(NAME physics.${name}
      COMMAND tracecmp ${file} ${SCREAMER_DATA_DIR}/GFX/MAP1H.DAT MAP1 --data ${SCREAMER_DATA_DIR}/DATA/MAP1.DAT --exact)
    set_tests_properties(physics.${name} PROPERTIES LABELS "physics")
  endif()
endforeach()

# The original's replays (the user's REPLAYS folder) played in ours against recordings of the
# original playing them (tools/dosbox/record-replay.ps1): positions of the player and every
# CPU car, bit for bit, every frame. name:replay:track:offset:frames
foreach(spec replay01:REPLAY01:MAP1:0:5640 replay11a:REPLAY11:MAP2:0:7810 replay11b:REPLAY11:MAP2:5400:7810)
  string(REPLACE ":" ";" parts ${spec})
  list(GET parts 0 name)
  list(GET parts 1 rpl)
  list(GET parts 2 track)
  list(GET parts 3 offset)
  list(GET parts 4 frames)
  set(file ${CMAKE_SOURCE_DIR}/artifacts/traces/${name}.trace)
  if(EXISTS ${file} AND EXISTS ${SCREAMER_DATA_DIR}/REPLAYS/${rpl}.RPL)
    add_test(NAME physics.${name}
      COMMAND ${CMAKE_COMMAND} -DEXE=$<TARGET_FILE:screamer> -DTRACECMP=$<TARGET_FILE:tracecmp>
        -DDATA_DIR=${SCREAMER_DATA_DIR} -DRPL=${SCREAMER_DATA_DIR}/REPLAYS/${rpl}.RPL -DTRACK=${track}
        -DTRACE=${file} -DOFFSET=${offset} -DFRAMES=${frames} -DOUTPUT=${TEST_OUT_DIR}/${name}.positions
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/replay_positions.cmake)
    set_tests_properties(physics.${name} PROPERTIES LABELS "physics" TIMEOUT 900)
  endif()
endforeach()

# --- Original game data: verify the user's copy against the recorded manifest ---
add_test(NAME gamedata.manifest
  COMMAND ${CMAKE_COMMAND}
    -DDATA_DIR=${SCREAMER_DATA_DIR}
    -DMANIFEST=${CMAKE_SOURCE_DIR}/reference/manifest/screamer_files.sha256
    -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_manifest.cmake)
set_tests_properties(gamedata.manifest PROPERTIES LABELS "gamedata" SKIP_RETURN_CODE 77)
