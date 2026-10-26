// miniaudio와 stb_vorbis 구현 (외부 라이브러리 코드라 경고를 끄고 빌드한다: CMakeLists.txt)
#define MINIAUDIO_IMPLEMENTATION
#include "MiniaudioConfig.h"

#undef STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"
