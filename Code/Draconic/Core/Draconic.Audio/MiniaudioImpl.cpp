// Draconic::Audio - the ONE translation unit that compiles the miniaudio implementation
// (plus stb_vorbis for Ogg Vorbis, which miniaudio picks up automatically when its
// header-only part is visible before the implementation). A plain TU, not a module unit:
// vendored code stays out of the module graph entirely (GCC module hygiene) and builds
// without our -Werror expectations applying to third-party warnings suppressed below.

#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wsign-compare"
#endif
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wunused-but-set-variable"
#pragma clang diagnostic ignored "-Wshift-op-parentheses"
#pragma clang diagnostic ignored "-Wtautological-compare"
#endif

#define STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c" // header-only pass: enables miniaudio's built-in vorbis backend

#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#undef STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c" // implementation pass
