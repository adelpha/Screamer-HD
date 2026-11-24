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
