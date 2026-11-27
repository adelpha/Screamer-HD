# The audio decoders for ripped CD music (third-party single-file libraries; warnings off).
add_library(audio_decoders STATIC core/audio_decoders.c)
target_include_directories(audio_decoders SYSTEM PUBLIC ${dr_libs_SOURCE_DIR} ${stb_SOURCE_DIR})
if(MSVC)
  target_compile_options(audio_decoders PRIVATE /W0)
else()
  target_compile_options(audio_decoders PRIVATE -w)
endif()

# Engine-side code that is independent of the window/frontend. Unit tests link this.
add_library(screamer_core STATIC
  core/animations.cpp
  core/archive.cpp
  core/car.cpp
  core/controls.cpp
  core/dtr.cpp
  core/framebuffer.cpp
  core/hud.cpp
  core/gamedata.cpp
  core/image.cpp
  core/menu.cpp
  core/mipmaps.cpp
  core/model3d.cpp
  core/obstacles.cpp
  core/opponent.cpp
  core/race_audio.cpp
  core/sound.cpp
  core/cd_audio.cpp
  core/fm_synth.cpp
  core/pcx.cpp
  core/render3d.cpp
  core/render3d_fill_sse2.cpp
  core/render3d_fill_avx2.cpp
  core/worker_pool.cpp
  core/tv_camera.cpp
  core/profiler.cpp
  core/attract.cpp
  core/replay.cpp
  core/sim_clock.cpp
  core/testpattern.cpp
  core/track.cpp
)
# The AVX2 build of the pixel filler: this file only, used when the CPU has AVX2.
if(MSVC)
  set_source_files_properties(core/render3d_fill_avx2.cpp PROPERTIES COMPILE_OPTIONS "/arch:AVX2")
else()
  set_source_files_properties(core/render3d_fill_avx2.cpp PROPERTIES COMPILE_OPTIONS "-mavx2")
endif()
target_include_directories(screamer_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(screamer_core PUBLIC SDL3::SDL3 nuked_opl3)
target_link_libraries(screamer_core PRIVATE audio_decoders)
if(SCREAMER_DIST)
  set(SCREAMER_EMBEDDED_DATA_DIR "")  # a package looks next to itself, not on the build machine
else()
  set(SCREAMER_EMBEDDED_DATA_DIR "${SCREAMER_DATA_DIR}")
endif()
target_compile_definitions(screamer_core PUBLIC
  SCREAMER_DEFAULT_DATA_DIR="${SCREAMER_EMBEDDED_DATA_DIR}"
  SCREAMER_VERSION="${PROJECT_VERSION}"
)

# The game executable: console subsystem, so headless runs log to stdout; in release packages
# (SCREAMER_DIST) a Windows-subsystem program, which takes its parent's console for logs
# (attach_parent_console) and pipes as given.
add_executable(screamer main.cpp input.cpp debug_report.cpp)
target_link_libraries(screamer PRIVATE screamer_core)
if(SCREAMER_DIST AND WIN32)
  set_target_properties(screamer PROPERTIES WIN32_EXECUTABLE ON)
  target_compile_definitions(screamer PRIVATE SCREAMER_GUI_SUBSYSTEM=1)
endif()

# Image comparison tool used by the test harness and the DOSBox reference scripts.
add_executable(imgcmp tools/imgcmp.cpp debug_report.cpp)
target_link_libraries(imgcmp PRIVATE screamer_core)
add_executable(tracecmp tools/tracecmp.cpp debug_report.cpp)
target_link_libraries(tracecmp PRIVATE screamer_core)
