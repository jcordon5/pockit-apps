/* stb_image (dominio público), compilada una sola vez para todo Pockit: PNG, JPEG y BMP para
 * los fondos, y su inflate para leer los ePub (que son zip). */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_HDR
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#include "../../common/stb_image.h"

unsigned long long fondos_hash(const char *s)
{
    unsigned long long h = 1469598103934665603ULL;
    for (; *s; s++) { h ^= (unsigned char)*s; h *= 1099511628211ULL; }
    return h;
}
