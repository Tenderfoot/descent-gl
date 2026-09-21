// Single translation unit holding the stb implementations.
//
// stb_image's loader went unused when M2 replaced the last file-backed texture
// with procedurally generated tiles, and stayed compiled in on the grounds that
// the first hand-authored texture would want it back. It did: the wall tiles are
// PNGs again, read by wall_tiles.cpp.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_FAILURE_USERMSG
#include <stb_image.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>
