include(FetchContent)

# SDL3: graphics, audio, input. Pinned to an exact release tag.
set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
  SDL3
  GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
  GIT_TAG        release-3.4.20
  GIT_SHALLOW    TRUE
  SYSTEM
)
FetchContent_MakeAvailable(SDL3)

# Nuked-OPL3 (fast fork): cycle-accurate Yamaha OPL3 for the MIDI music's FM sound.
# LGPL-2.1, so it is built as its own shared library (opl3.dll) next to the game; the source
# comes from the pinned commit at build time.
FetchContent_Declare(
  nuked_opl3
  GIT_REPOSITORY https://github.com/tgies/Nuked-OPL3-fast.git
  GIT_TAG        f44bacb1143cd78d36bacf69fbec8c60a125b3c9
  SYSTEM
)
FetchContent_MakeAvailable(nuked_opl3)  # no CMakeLists.txt there: this only fetches
add_library(nuked_opl3 SHARED ${nuked_opl3_SOURCE_DIR}/opl3.c)
set_target_properties(nuked_opl3 PROPERTIES OUTPUT_NAME opl3 WINDOWS_EXPORT_ALL_SYMBOLS ON C_STANDARD 99)
target_include_directories(nuked_opl3 SYSTEM PUBLIC ${nuked_opl3_SOURCE_DIR})
if(MSVC)
  target_compile_options(nuked_opl3 PRIVATE /W0)
endif()

# Decoders for CD music ripped to files (the MUSIC folder): dr_mp3, dr_flac and dr_wav from
# dr_libs, and stb_vorbis from stb. Single-file libraries, public domain or MIT-0/MIT; only fetched
# here, and compiled into the game by audio_decoders (src/CMakeLists.txt).
FetchContent_Declare(
  dr_libs
  GIT_REPOSITORY https://github.com/mackron/dr_libs.git
  GIT_TAG        dfbebc36e7a17a0f9511798c1bd77d96f0e74413
  SOURCE_SUBDIR  no-cmake  # its CMakeLists.txt builds its tests; only the headers are wanted
  SYSTEM
)
FetchContent_Declare(
  stb
  GIT_REPOSITORY https://github.com/nothings/stb.git
  GIT_TAG        6b5799bae8a1fe3792691ef4b72ae26434c3e799
  SYSTEM
)
FetchContent_MakeAvailable(dr_libs stb)
