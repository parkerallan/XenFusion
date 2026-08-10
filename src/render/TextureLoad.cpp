#include "render/Mesh.h"

#include <vector>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO_DEPRECATED
#include "stb/stb_image.h"

namespace mesh
{
    IDirect3DTexture9* LoadTexture(IDirect3DDevice9* device, const std::filesystem::path& path, AlphaKind* out_alpha, bool* out_height)
    {
        if (out_alpha)
            *out_alpha = AlphaKind::Opaque;
        if (out_height)
            *out_height = false;
        if (!device)
            return nullptr;

        int w = 0, h = 0, channels = 0;
        unsigned char* pixels = stbi_load(path.string().c_str(), &w, &h, &channels, 4); // force RGBA
        if (!pixels)
            return nullptr;

        // Classify the alpha channel from the pixels, so the material picks the
        // right transparency mode with no inspector toggle. The distinction is
        // "does it have real see-through holes" — not "is the alpha binary", since
        // a hair card's alpha is mostly soft (anti-aliased strands):
        //   - no alpha < 255                 -> Opaque
        //   - fully-transparent holes present -> Cutout (a mask: hair, foliage,
        //     fences) -> alpha-test / alpha-to-coverage, depth-correct, no sorting
        //   - uniformly translucent, no holes -> Blend  (glass, water)
        if (out_alpha && channels == 4)
        {
            int total       = w * h;
            int translucent = 0; // any alpha < 255
            int transparent = 0; // alpha ~0: a real see-through hole
            for (int i = 0; i < total; ++i)
            {
                unsigned char a = pixels[i * 4 + 3];
                if (a < 255) ++translucent;
                if (a < 24)  ++transparent;
            }
            if (translucent == 0)
                *out_alpha = AlphaKind::Opaque;
            else if (transparent > total / 32) // >~3% see-through -> masked cutout
                *out_alpha = AlphaKind::Cutout;
            else
                *out_alpha = AlphaKind::Blend;
        }

        // Does the alpha channel carry a height field (bump offset — normal
        // maps pack it in their alpha, 0.5 = neutral)? Only if it actually
        // varies: a normal map without an alpha channel (or with a blank one)
        // carries no relief, and treating its uniform alpha as height would
        // shift every texture of the material by a constant UV offset.
        if (out_height && channels == 4)
        {
            unsigned char amin = 255, amax = 0;
            const int total = w * h;
            for (int i = 0; i < total; ++i)
            {
                unsigned char a = pixels[i * 4 + 3];
                if (a < amin) amin = a;
                if (a > amax) amax = a;
            }
            *out_height = (amax - amin) >= 8;
        }

        // Alpha dilation for cutout masks. The fully-transparent texels usually
        // carry a stray background color (this hair's is a light tan), and
        // bilinear filtering smears it into every kept strand edge — the white
        // fringe. Flood the covered color outward into the transparent texels
        // (alpha left untouched) so filtering blends hair<->hair. A few rings
        // cover the bilinear footprint; this makes any hair/foliage texture work
        // without hand-editing it.
        if (out_alpha && *out_alpha == AlphaKind::Cutout)
        {
            const int N = w * h;
            std::vector<unsigned char> covered(N);
            for (int i = 0; i < N; ++i)
                covered[i] = pixels[i * 4 + 3] >= 16 ? 1 : 0;
            std::vector<unsigned char> snap = covered; // read from last pass's state
            for (int pass = 0; pass < 6; ++pass)
            {
                bool grew = false;
                for (int y = 0; y < h; ++y)
                    for (int x = 0; x < w; ++x)
                    {
                        const int idx = y * w + x;
                        if (snap[idx]) continue;
                        int from = -1;
                        for (int dy = -1; dy <= 1 && from < 0; ++dy)
                            for (int dx = -1; dx <= 1; ++dx)
                            {
                                if (!dx && !dy) continue;
                                const int nx = x + dx, ny = y + dy;
                                if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
                                const int nidx = ny * w + nx;
                                if (snap[nidx]) { from = nidx; break; }
                            }
                        if (from >= 0)
                        {
                            pixels[idx * 4 + 0] = pixels[from * 4 + 0];
                            pixels[idx * 4 + 1] = pixels[from * 4 + 1];
                            pixels[idx * 4 + 2] = pixels[from * 4 + 2];
                            covered[idx] = 1;
                            grew = true;
                        }
                    }
                snap = covered;
                if (!grew) break;
            }
        }

        IDirect3DTexture9* tex = CreateTextureFromRgba(device, pixels, w, h);
        stbi_image_free(pixels);
        return tex;
    }

    IDirect3DTexture9* CreateTextureFromRgba(IDirect3DDevice9* device,
                                             const unsigned char* pixels, int w, int h)
    {
        if (!device || !pixels || w <= 0 || h <= 0)
            return nullptr;

        IDirect3DTexture9* tex = nullptr;
        if (FAILED(device->CreateTexture((UINT)w, (UINT)h, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex, nullptr)))
            return nullptr;

        D3DLOCKED_RECT rect;
        if (SUCCEEDED(tex->LockRect(0, &rect, nullptr, 0)))
        {
            for (int y = 0; y < h; ++y)
            {
                unsigned char*       dst = static_cast<unsigned char*>(rect.pBits) + (size_t)y * rect.Pitch;
                const unsigned char* src = pixels + (size_t)y * w * 4;
                for (int x = 0; x < w; ++x)
                {
                    // stb gives RGBA; D3DFMT_A8R8G8B8 is BGRA in memory.
                    dst[x * 4 + 0] = src[x * 4 + 2]; // B
                    dst[x * 4 + 1] = src[x * 4 + 1]; // G
                    dst[x * 4 + 2] = src[x * 4 + 0]; // R
                    dst[x * 4 + 3] = src[x * 4 + 3]; // A
                }
            }
            tex->UnlockRect(0);
        }
        return tex;
    }

    IDirect3DCubeTexture9* LoadEnvCube(IDirect3DDevice9* device, const std::filesystem::path& dir)
    {
        if (!device)
            return nullptr;

        // Face order matches D3DCUBEMAP_FACE_POSITIVE_X .. NEGATIVE_Z.
        static const char* kFaces[6] =
            { "env_px.png", "env_nx.png", "env_py.png", "env_ny.png", "env_pz.png", "env_nz.png" };

        unsigned char* pixels[6] = {};
        int size = 0;
        for (int f = 0; f < 6; ++f)
        {
            int w = 0, h = 0, ch = 0;
            pixels[f] = stbi_load((dir / kFaces[f]).string().c_str(), &w, &h, &ch, 4);
            if (!pixels[f] || w != h || (size != 0 && w != size))
            {
                for (int k = 0; k <= f; ++k) stbi_image_free(pixels[k]);
                return nullptr; // missing or mismatched face -> no env map
            }
            size = w;
        }

        IDirect3DCubeTexture9* cube = nullptr;
        if (FAILED(device->CreateCubeTexture((UINT)size, 1, 0, D3DFMT_A8R8G8B8,
                                             D3DPOOL_MANAGED, &cube, nullptr)))
        {
            for (int f = 0; f < 6; ++f) stbi_image_free(pixels[f]);
            return nullptr;
        }
        for (int f = 0; f < 6; ++f)
        {
            D3DLOCKED_RECT rect;
            if (SUCCEEDED(cube->LockRect((D3DCUBEMAP_FACES)f, 0, &rect, nullptr, 0)))
            {
                for (int y = 0; y < size; ++y)
                {
                    unsigned char*       dst = static_cast<unsigned char*>(rect.pBits) + (size_t)y * rect.Pitch;
                    const unsigned char* src = pixels[f] + (size_t)y * size * 4;
                    for (int x = 0; x < size; ++x)
                    {
                        dst[x * 4 + 0] = src[x * 4 + 2]; // B
                        dst[x * 4 + 1] = src[x * 4 + 1]; // G
                        dst[x * 4 + 2] = src[x * 4 + 0]; // R
                        dst[x * 4 + 3] = src[x * 4 + 3]; // A
                    }
                }
                cube->UnlockRect((D3DCUBEMAP_FACES)f, 0);
            }
            stbi_image_free(pixels[f]);
        }
        return cube;
    }
}
