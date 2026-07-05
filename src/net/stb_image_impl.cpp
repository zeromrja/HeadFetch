// Single translation unit providing the stb_image implementation.
// stb_image.h is a header-only public-domain PNG/JPEG decoder; we only use
// its PNG decoding path here (see AvatarFetcher.h).
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include <stb_image.h>
