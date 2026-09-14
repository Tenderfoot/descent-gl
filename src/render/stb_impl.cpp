// Single translation unit holding the stb implementations.
//
// stb_image's loader is unused since M2 replaced the last file-backed texture
// with procedurally generated tiles. It stays because it costs nothing here and
// the first hand-authored texture will want it back.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_FAILURE_USERMSG
#include <stb_image.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>
