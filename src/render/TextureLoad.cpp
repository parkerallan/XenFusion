#include "render/Mesh.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO_DEPRECATED
#include "stb/stb_image.h"

namespace mesh
{
    IDirect3DTexture9* LoadTexture(IDirect3DDevice9* device, const std::filesystem::path& path)
    {
        if (!device)
            return nullptr;

        int w = 0, h = 0, channels = 0;
        unsigned char* pixels = stbi_load(path.string().c_str(), &w, &h, &channels, 4); // force RGBA
        if (!pixels)
            return nullptr;

        IDirect3DTexture9* tex = nullptr;
        if (FAILED(device->CreateTexture((UINT)w, (UINT)h, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex, nullptr)))
        {
            stbi_image_free(pixels);
            return nullptr;
        }

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

        stbi_image_free(pixels);
        return tex;
    }
}
