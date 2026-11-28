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
  unit/test_track_edit.cpp
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

# --- The main menu's way out on a pad (needs the game data): Back quits at once, B held 2 s on a
# pad without Back quits after 100 frames, a tap of B does nothing ---
if(EXISTS ${SCREAMER_DATA_DIR}/GFX/MENU.DAT)
  add_test(NAME smoke.menu_pad_back COMMAND screamer --headless --scene menu --frames 400 --menu-keys 200:padback)
  set_tests_properties(smoke.menu_pad_back PROPERTIES LABELS "smoke" PASS_REGULAR_EXPRESSION "frames=201")
  add_test(NAME smoke.menu_pad_hold_b COMMAND screamer --headless --scene menu --frames 400 --menu-keys 200-330:padb)
  set_tests_properties(smoke.menu_pad_hold_b PROPERTIES LABELS "smoke" PASS_REGULAR_EXPRESSION "frames=301")
  add_test(NAME smoke.menu_pad_tap_b COMMAND screamer --headless --scene menu --frames 400 --menu-keys 200-205:padb)
  set_tests_properties(smoke.menu_pad_tap_b PROPERTIES LABELS "smoke" PASS_REGULAR_EXPRESSION "frames=400")
endif()

# --- The OpenGL presenter (--video-api opengl) against SDL's renderer, both on the graphics card
# through a hidden window that is never shown (--hidden). Skipped without OpenGL 3.3. ---
function(add_api_test name)
  cmake_parse_arguments(AT "" "TOLERANCE;MAX_FRACTION;EXPECT" "ARGS" ${ARGN})
  string(REPLACE ";" "|" AT_ARGS_PIPE "${AT_ARGS}")
  add_test(NAME gpu.${name}
    COMMAND ${CMAKE_COMMAND}
      -DEXE=$<TARGET_FILE:screamer>
      -DIMGCMP=$<TARGET_FILE:imgcmp>
      "-DARGS=${AT_ARGS_PIPE}"
      -DOUTPUT=${TEST_OUT_DIR}/api_${name}
      -DTOLERANCE=${AT_TOLERANCE}
      -DMAX_FRACTION=${AT_MAX_FRACTION}
      "-DEXPECT=${AT_EXPECT}"
      -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/compare_apis.cmake)
  set_tests_properties(gpu.${name} PROPERTIES LABELS "gpu" SKIP_RETURN_CODE 77)
endfunction()
add_api_test(testpattern_640x480 ARGS --scene testpattern --frames 1 --size 640x480)
add_api_test(testpattern_1280x720 ARGS --scene testpattern --frames 1 --size 1280x720)
if(EXISTS ${SCREAMER_DATA_DIR}/GFX/MAP1H.DAT)
  # the picture larger than the window: converted, halved twice, then filtered (exact); and
  # neither halved nor whole (the linear filter's rounding: +-1)
  add_api_test(track_supersampled ARGS --scene track --track MAP1 --frames 3 --size 1280x720 --render-size 2560x1600)
  add_api_test(track_filtered ARGS --scene track --track MAP3 --frames 3 --size 1366x768 --render-size 1601x1001
    TOLERANCE 1)
  # 320x200 shown 4.5 times as large: SDL's renderer and OpenGL round some exact texel edges apart
  add_api_test(track_upscaled ARGS --scene track --track MAP1 --frames 3 --size 1920x1080 --render-size 320x200
    MAX_FRACTION 0.005)
  add_api_test(race_wide ARGS --scene drive --track MAP1 --frames 300 --size 1280x720 --aspect wide --hold accel)
endif()
# The hardware renderer (--renderer3d hardware) against the software renderer: the same picture
# but for texel and pixel edges rounded apart (mipmaps off), and with mipmaps on, the levels the
# graphics card picks (one for both directions; the software renderer halves u and v apart).
function(add_renderer_test name)
  cmake_parse_arguments(RT "" "TOLERANCE;MAX_FRACTION" "ARGS" ${ARGN})
  string(REPLACE ";" "|" RT_ARGS_PIPE "${RT_ARGS}")
  add_test(NAME gpu.hw_${name}
    COMMAND ${CMAKE_COMMAND}
      -DEXE=$<TARGET_FILE:screamer>
      -DIMGCMP=$<TARGET_FILE:imgcmp>
      "-DARGS=${RT_ARGS_PIPE}"
      -DOUTPUT=${TEST_OUT_DIR}/hw_${name}
      -DTOLERANCE=${RT_TOLERANCE}
      -DMAX_FRACTION=${RT_MAX_FRACTION}
      -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/compare_renderers.cmake)
  set_tests_properties(gpu.hw_${name} PROPERTIES LABELS "gpu" SKIP_RETURN_CODE 77)
endfunction()
foreach(n 1 3 5 6)
  if(EXISTS ${SCREAMER_DATA_DIR}/GFX/MAP${n}H.DAT)
    add_renderer_test(race_map${n} ARGS --scene drive --track MAP${n} --hold accel --frames 340 --render-size 1600x1000
      --mipmaps off TOLERANCE 24 MAX_FRACTION 0.01)
  endif()
endforeach()
if(EXISTS ${SCREAMER_DATA_DIR}/GFX/MAP1H.DAT)
  add_renderer_test(race_mipmaps ARGS --scene drive --track MAP1 --hold accel --frames 300 --render-size 1280x800
    TOLERANCE 24 MAX_FRACTION 0.05)
  add_renderer_test(still_cars ARGS --scene track --track MAP1 --car AUTO00 --car AUTO03,0,0,0,300,8000 --frames 3
    --render-size 1600x1000 --mipmaps off TOLERANCE 24 MAX_FRACTION 0.005)
  add_renderer_test(mirror_wide ARGS --scene drive --track MAP1 --hold accel,right --frames 200 --render-size 1280x720
    --aspect wide --race-options 0x30 --mipmaps off TOLERANCE 24 MAX_FRACTION 0.02)
endif()

if(EXISTS ${SCREAMER_DATA_DIR}/GFX/MENU.DAT)
  # the widescreen menu's backdrop
  add_api_test(menu_wide ARGS --scene menu --frames 120 --size 1920x1080 --aspect wide)
  # OPTIONS > VIDEO > ADVANCED > API, from DIRECT3D to OPENGL: the window made again, live
  add_test(NAME gpu.menu_api_switch COMMAND screamer --hidden --scene menu --frames 1000 --size 1280x720 --menu-keys
    100:down,130:down,160:down,190:down,240:enter,320:down,350:down,400:enter,470:down,495:down,520:down,545:down,570:down,600:down,640:enter,720:down,750:down,780:down,810:down,860:enter)
  set_tests_properties(gpu.menu_api_switch PROPERTIES LABELS "gpu"
    PASS_REGULAR_EXPRESSION "renderer: opengl[^
]*
(.*
)?frames=1000")
endif()

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
