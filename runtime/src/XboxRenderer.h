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
    // Replay + resolve the tiles into the front buffer, run the glow chain over
    // it (when its shaders are set), and swap it to screen.
    void EndFrame();

    // Emissive glow (bloom) post shaders, run between the tile resolve and Swap.
    // The scene pass exports each pixel's emissive strength in the front
    // buffer's ALPHA channel (standard.hlsl c15); the chain extracts rgb * a at
    // quarter res, blurs it wide, and adds the halo back over the frame.
    // Pointers are borrowed — SceneRuntime's Content owns the shaders.
    void SetBloomShaders(IDirect3DVertexShader9* brightVS, IDirect3DPixelShader9* brightPS,
                         IDirect3DVertexShader9* blurVS,   IDirect3DPixelShader9* blurPS,
                         IDirect3DVertexShader9* combineVS, IDirect3DPixelShader9* combinePS);

    void SetClearColor(float r, float g, float b);

    IDirect3DDevice9* Device() const { return m_device; }
    int Width()  const { return m_width; }
    int Height() const { return m_height; }

private:
    // One full-screen pass of the glow chain: bind dst + viewport, upload the
    // half-pixel VS constant, draw a textured quad with vs/ps sampling src.
    void GlowPass(IDirect3DSurface9* dst, int dw, int dh,
                  IDirect3DVertexShader9* vs, IDirect3DPixelShader9* ps,
                  IDirect3DBaseTexture9* src);
    void RenderGlow(); // the whole chain: extract -> blur H -> blur V -> add

    IDirect3D9*        m_d3d;
    IDirect3DDevice9*  m_device;
    IDirect3DSurface9* m_tileRT;    // MSAA colour target, one tile's worth, in EDRAM
    IDirect3DSurface9* m_tileDepth; // matching MSAA depth, in EDRAM
    IDirect3DTexture9* m_front[2];  // double-buffered front buffers for Swap
    int                m_frontIdx;
    float              m_clearR, m_clearG, m_clearB;
    int                m_width, m_height;

    // Glow chain resources. The EDRAM surfaces alias the tile targets (Base 0):
    // the chain only runs after EndTiling, when EDRAM is free again.
    IDirect3DSurface9* m_glowSmallRT; // quarter-res working target, in EDRAM
    IDirect3DSurface9* m_glowFullRT;  // full-screen composite target, in EDRAM
    IDirect3DTexture9* m_glowTexA;    // quarter-res ping
    IDirect3DTexture9* m_glowTexB;    // quarter-res pong
    IDirect3DVertexShader9* m_glowBrightVS;
    IDirect3DPixelShader9*  m_glowBrightPS;
    IDirect3DVertexShader9* m_glowBlurVS;
    IDirect3DPixelShader9*  m_glowBlurPS;
    IDirect3DVertexShader9* m_glowCombineVS;
    IDirect3DPixelShader9*  m_glowCombinePS;
};
