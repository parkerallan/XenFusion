#pragma once

// The editor writes its baked blobs (.mesh) and text (.scene) on a little-endian
// PC. The Xbox 360 / Xenia is big-endian PowerPC, so every multi-byte scalar
// read from those files must be assembled from little-endian bytes rather than
// reinterpreted. These helpers assemble from explicit byte order, so they are
// correct regardless of host endianness (no reinterpret_cast of the raw bytes).

#include <string.h>

namespace endian
{
    inline unsigned short LoadU16LE(const unsigned char* p)
    {
        return (unsigned short)((unsigned)p[0] | ((unsigned)p[1] << 8));
    }

    inline unsigned int LoadU32LE(const unsigned char* p)
    {
        return (unsigned int)p[0] | ((unsigned int)p[1] << 8) |
               ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
    }

    inline float LoadF32LE(const unsigned char* p)
    {
        unsigned int u = LoadU32LE(p);
        float f;
        memcpy(&f, &u, sizeof(f));
        return f;
    }

    // Store helpers write explicit little-endian bytes regardless of host order.
    // Procedurally-built vertex data must be little-endian to match the shipped
    // mesh blobs: the 360 vertex-fetch format (D3DDECLTYPE_FLOAT*) byte-swaps
    // 32-bit words on fetch, so buffers are authored little-endian everywhere.
    inline void StoreU32LE(unsigned char* p, unsigned int u)
    {
        p[0] = (unsigned char)(u & 0xFF);
        p[1] = (unsigned char)((u >> 8) & 0xFF);
        p[2] = (unsigned char)((u >> 16) & 0xFF);
        p[3] = (unsigned char)((u >> 24) & 0xFF);
    }

    inline void StoreF32LE(unsigned char* p, float f)
    {
        unsigned int u;
        memcpy(&u, &f, sizeof(u));
        StoreU32LE(p, u);
    }

    // Copy 32-bit words from a little-endian source (the editor's .mesh blobs) to
    // native byte order. Xenia's vertex/index fetch reads buffers in the console's
    // NATIVE (big-endian) order — it does not apply the declaration's 8IN32 swap —
    // so GPU buffers must hold native-endian floats and indices, not little-endian.
    inline void StoreNativeFromLE32(void* dst, const void* src, size_t bytes)
    {
        unsigned char*       d = (unsigned char*)dst;
        const unsigned char* s = (const unsigned char*)src;
        for (size_t k = 0; k + 4 <= bytes; k += 4)
        {
            unsigned int u = LoadU32LE(s + k); // native value from LE bytes
            memcpy(d + k, &u, 4);              // store in native byte order
        }
    }
}
