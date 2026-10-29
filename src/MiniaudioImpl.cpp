// miniaudio and stb_vorbis implementations (external library code, so built with warnings off: CMakeLists.txt)
#define MINIAUDIO_IMPLEMENTATION
#include "MiniaudioConfig.h"

#undef STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"
