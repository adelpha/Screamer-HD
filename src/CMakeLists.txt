# Engine-side code that is independent of the window/frontend. Unit tests link this.
add_library(screamer_core STATIC
  core/animations.cpp
  core/archive.cpp
  core/car.cpp
  core/dtr.cpp
  core/framebuffer.cpp
  core/hud.cpp
  core/gamedata.cpp
  core/image.cpp
  core/menu.cpp
  core/model3d.cpp
  core/obstacles.cpp
  core/opponent.cpp
  core/race_audio.cpp
  core/sound.cpp
  core/cd_audio.cpp
  core/fm_synth.cpp
  core/pcx.cpp
  core/render3d.cpp
  core/worker_pool.cpp
  core/tv_camera.cpp
  core/profiler.cpp
  core/attract.cpp
  core/replay.cpp
  core/sim_clock.cpp
  core/testpattern.cpp
  core/track.cpp
)
target_include_directories(screamer_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(screamer_core PUBLIC SDL3::SDL3 nuked_opl3)
target_compile_definitions(screamer_core PUBLIC
  SCREAMER_DEFAULT_DATA_DIR="${SCREAMER_DATA_DIR}"
  SCREAMER_VERSION="${PROJECT_VERSION}"
)

# The game executable (console subsystem for now so headless runs can log to stdout).
add_executable(screamer main.cpp debug_report.cpp)
target_link_libraries(screamer PRIVATE screamer_core)

# Image comparison tool used by the test harness and the DOSBox reference scripts.
add_executable(imgcmp tools/imgcmp.cpp debug_report.cpp)
target_link_libraries(imgcmp PRIVATE screamer_core)
add_executable(tracecmp tools/tracecmp.cpp debug_report.cpp)
target_link_libraries(tracecmp PRIVATE screamer_core)
