/* Single translation unit that compiles the stb_vorbis implementation.
   audio.cpp includes stb_vorbis.c with STB_VORBIS_HEADER_ONLY for just the
   declarations; this file compiles the full decoder once. */
#include "stb_vorbis.c"
