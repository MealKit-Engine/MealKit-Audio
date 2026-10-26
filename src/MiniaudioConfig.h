#pragma once

// miniaudio 설정 (mkaudio 안에서만 include한다. 이름이 miniaudio.h와 겹치지 않게). 구현은 MiniaudioImpl.cpp 하나.
// 디코더: WAV(효과음, 메모리에 풀어 둔다)와 Ogg Vorbis(배경음 스트리밍, stb_vorbis). MP3·FLAC·인코딩·생성기는 뺀다.
// 장치 백엔드는 플랫폼마다 하나 (Windows WASAPI). 다른 플랫폼은 5단계에서 그 플랫폼 백엔드를 켠다.
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
