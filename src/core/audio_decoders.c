/* The implementations of the single-file audio decoders used for ripped CD music
   (core/cd_audio.cpp): dr_mp3, dr_flac and dr_wav by David Reid, stb_vorbis by Sean Barrett.
   All are public domain (or MIT-0 / MIT) and fetched at a pinned commit (cmake/Dependencies.cmake).
   Built on its own with warnings off, since this is third-party code. */
#define DR_MP3_IMPLEMENTATION
#include "dr_mp3.h"
#define DR_FLAC_IMPLEMENTATION
#define DR_FLAC_NO_OGG
#include "dr_flac.h"
#define DR_WAV_IMPLEMENTATION
#include "dr_wav.h"
#define STB_VORBIS_NO_PUSHDATA_API
#include "stb_vorbis.c"
