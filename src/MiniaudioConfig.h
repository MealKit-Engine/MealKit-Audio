#pragma once

// miniaudio configuration (included only inside mkaudio. Named so it doesn't collide with miniaudio.h). The implementation is the single MiniaudioImpl.cpp.
// Decoders: WAV (sound effects, decoded into memory) and Ogg Vorbis (music streaming, stb_vorbis). MP3/FLAC/encoding/generators are excluded.
// One device backend per platform (Windows WASAPI). Other platforms enable their backend in stage 5.
#define MA_NO_FLAC
#define MA_NO_MP3
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MA_ENABLE_ONLY_SPECIFIC_BACKENDS
#if defined(_WIN32)
#define MA_ENABLE_WASAPI
#elif defined(__APPLE__)
#define MA_ENABLE_COREAUDIO
#else
#define MA_ENABLE_PULSEAUDIO
#define MA_ENABLE_ALSA
#endif

#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#define STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"
#include "miniaudio.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
