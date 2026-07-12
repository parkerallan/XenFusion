#pragma once

#include <xtl.h>

// The console analogue of the editor's Renderer class: the D3D9x porting seam.
//
// Uses PREDICATED TILING so the scene can render at full 1280x720 with MSAA (and
// thus alpha-to-coverage) even though a 720p multisampled colour+depth surface
// overflows the 360's 10MB EDRAM. The scene is rendered once between
// BeginTiling/EndTiling; the runtime replays it per screen tile (each tile fits
// EDRAM) and resolves the tiles into a full-screen front buffer that Swap
// displays. There is no auto back buffer — presentation is fully custom.
class XboxRenderer
{
public:
    XboxRenderer();

    bool Init();
    void Shutdown();

    // Bind the tiling MSAA target and begin a tiled frame (clears each tile).
    bool BeginFrame();
    // Replay + resolve the tiles into the front buffer and swap it to screen.
    void EndFrame();

    void SetClearColor(float r, float g, float b);

    IDirect3DDevice9* Device() const { return m_device; }
    int Width()  const { return m_width; }
    int Height() const { return m_height; }

private:
    IDirect3D9*        m_d3d;
    IDirect3DDevice9*  m_device;
    IDirect3DSurface9* m_tileRT;    // MSAA colour target, one tile's worth, in EDRAM
    IDirect3DSurface9* m_tileDepth; // matching MSAA depth, in EDRAM
    IDirect3DTexture9* m_front[2];  // double-buffered front buffers for Swap
    int                m_frontIdx;
    float              m_clearR, m_clearG, m_clearB;
    int                m_width, m_height;
};
