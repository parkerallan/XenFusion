// stb_image implementation TU. Used by Content.cpp only to classify a diffuse
// texture's alpha channel (Opaque / Cutout / Blend), matching the editor. The
// actual GPU textures are still created by D3DX. STBI_NO_SIMD: the 360 is
// PowerPC, so the x86 SSE paths must be off.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_SIMD
#define STBI_NO_STDIO_DEPRECATED
#include "stb_image.h"
