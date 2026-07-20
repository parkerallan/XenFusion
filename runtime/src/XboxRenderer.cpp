#include "XboxRenderer.h"

#include <xgraphics.h> // XGNextMultiple
#include <stdio.h>

// Predicated-tiling Direct3D setup for the Xbox 360. See XboxRenderer.h.
//
// 1280x720 is split into two horizontal tiles; at 2x MSAA each tile's colour +
// depth fits the 10MB EDRAM. The scene is drawn once between BeginTiling and
// EndTiling, which replays the command buffer per tile and resolves each into a
// full-screen front buffer.

namespace
{
    const D3DMULTISAMPLE_TYPE kMSAA      = D3DMULTISAMPLE_4_SAMPLES;
    const DWORD               kTileCount = 3;
    // Three horizontal bands covering the 720p frame. The tile render target is
    // sized to the tallest band (256), which at 4x MSAA (colour+depth) fills the
    // 10MB EDRAM exactly.
    const D3DRECT kTileRects[3] = { { 0, 0, 1280, 256 }, { 0, 256, 1280, 512 }, { 0, 512, 1280, 720 } };
    const DWORD   kLargestTileW = 1280;
    const DWORD   kLargestTileH = 256;

    // Glow chain: quarter-res working size and the halo's strength/width.
    const int   kGlowW    = 320;
    const int   kGlowH    = 180;
    const float kGlowGain = 1.0f; // additive halo intensity
    const float kGlowStep = 1.5f; // blur tap spacing, in quarter-res texels
}

XboxRenderer::XboxRenderer()
    : m_d3d(NULL), m_device(NULL), m_tileRT(NULL), m_tileDepth(NULL),
      m_frontIdx(0), m_clearR(0.094f), m_clearG(0.094f), m_clearB(0.106f),
      m_width(1280), m_height(720),
      m_glowSmallRT(NULL), m_glowFullRT(NULL), m_glowTexA(NULL), m_glowTexB(NULL),
      m_glowBrightVS(NULL), m_glowBrightPS(NULL),
      m_glowBlurVS(NULL), m_glowBlurPS(NULL),
      m_glowCombineVS(NULL), m_glowCombinePS(NULL)
{
    m_front[0] = NULL;
    m_front[1] = NULL;
}

bool XboxRenderer::Init()
{
    m_d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!m_d3d)
        return false;

    XVIDEO_MODE vm;
    ZeroMemory(&vm, sizeof(vm));
    XGetVideoMode(&vm);
    m_width  = 1280;
    m_height = 720;

    // Custom present: we create our own front buffers and Swap them, so both the
    // auto back AND front buffers are disabled (leaving the auto front buffer on
    // makes the runtime scan out its empty buffer instead of ours -> black).
    D3DPRESENT_PARAMETERS pp;
    ZeroMemory(&pp, sizeof(pp));
    pp.BackBufferWidth        = m_width;
    pp.BackBufferHeight       = m_height;
    pp.BackBufferFormat       = (D3DFORMAT)MAKESRGBFMT(D3DFMT_A8R8G8B8);
    pp.MultiSampleType        = D3DMULTISAMPLE_NONE;
    pp.BackBufferCount        = 0;
    pp.EnableAutoDepthStencil = FALSE;   // we own the (MSAA) depth
    pp.DisableAutoBackBuffer  = TRUE;    // custom tiling present via Swap
    pp.DisableAutoFrontBuffer = TRUE;
    pp.AutoDepthStencilFormat = D3DFMT_D24S8;
    pp.SwapEffect             = D3DSWAPEFFECT_DISCARD;
    pp.PresentationInterval   = D3DPRESENT_INTERVAL_IMMEDIATE; // paced manually via Sync

    HRESULT hr = m_d3d->CreateDevice(0, D3DDEVTYPE_HAL, NULL,
                                     D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_BUFFER_2_FRAMES,
                                     &pp, &m_device);
    if (FAILED(hr) || !m_device)
        return false;

    // Tiling MSAA colour target (one tile) at EDRAM base 0, and a matching MSAA
    // depth placed immediately after the colour target in EDRAM.
    const DWORD tileW = XGNextMultiple(kLargestTileW, GPU_EDRAM_TILE_WIDTH_4X);
    const DWORD tileH = XGNextMultiple(kLargestTileH, GPU_EDRAM_TILE_HEIGHT_4X);

    D3DSURFACE_PARAMETERS sp;
    ZeroMemory(&sp, sizeof(sp));
    sp.Base = 0;
    if (FAILED(m_device->CreateRenderTarget(tileW, tileH, D3DFMT_A8R8G8B8, kMSAA, 0, FALSE, &m_tileRT, &sp)))
        return false;

    // Place the depth immediately after the colour target in EDRAM.
    sp.Base = m_tileRT->Size / GPU_EDRAM_TILE_SIZE;
    sp.HierarchicalZBase = 0;
    if (FAILED(m_device->CreateDepthStencilSurface(tileW, tileH, D3DFMT_D24S8, kMSAA, 0, FALSE, &m_tileDepth, &sp)))
        return false;

    // Two full-screen front buffers (double-buffered to avoid tearing) that Swap
    // scans out; the tiles resolve into these. A8 (not X8): the alpha channel
    // carries the emissive glow mask the bloom chain reads back (scanout
    // ignores alpha, so this is free).
    for (int i = 0; i < 2; ++i)
    {
        if (FAILED(m_device->CreateTexture(m_width, m_height, 1, 0,
                                           (D3DFORMAT)MAKESRGBFMT(D3DFMT_LE_A8R8G8B8),
                                           D3DPOOL_DEFAULT, &m_front[i], NULL)))
            return false;
    }
    m_frontIdx = 0;

    // Glow chain resources. The EDRAM surfaces are placed at Base 0, ALIASING
    // the tile targets — legal because the chain only runs after EndTiling,
    // when the tile contents have been resolved out. If anything here fails the
    // glow is simply skipped (shaders never get set without the targets).
    {
        D3DSURFACE_PARAMETERS gp;
        ZeroMemory(&gp, sizeof(gp));
        gp.Base = 0;
        m_device->CreateRenderTarget(kGlowW, kGlowH, D3DFMT_A8R8G8B8,
                                     D3DMULTISAMPLE_NONE, 0, FALSE, &m_glowSmallRT, &gp);
        gp.Base = 0;
        m_device->CreateRenderTarget(m_width, m_height,
                                     (D3DFORMAT)MAKESRGBFMT(D3DFMT_A8R8G8B8),
                                     D3DMULTISAMPLE_NONE, 0, FALSE, &m_glowFullRT, &gp);
        m_device->CreateTexture(kGlowW, kGlowH, 1, 0, D3DFMT_A8R8G8B8,
                                D3DPOOL_DEFAULT, &m_glowTexA, NULL);
        m_device->CreateTexture(kGlowW, kGlowH, 1, 0, D3DFMT_A8R8G8B8,
                                D3DPOOL_DEFAULT, &m_glowTexB, NULL);
    }
    return true;
}

void XboxRenderer::SetBloomShaders(IDirect3DVertexShader9* brightVS, IDirect3DPixelShader9* brightPS,
                                   IDirect3DVertexShader9* blurVS,   IDirect3DPixelShader9* blurPS,
                                   IDirect3DVertexShader9* combineVS, IDirect3DPixelShader9* combinePS)
{
    m_glowBrightVS  = brightVS;  m_glowBrightPS  = brightPS;
    m_glowBlurVS    = blurVS;    m_glowBlurPS    = blurPS;
    m_glowCombineVS = combineVS; m_glowCombinePS = combinePS;
}

void XboxRenderer::Shutdown()
{
    if (m_glowTexB)    { m_glowTexB->Release();    m_glowTexB = NULL; }
    if (m_glowTexA)    { m_glowTexA->Release();    m_glowTexA = NULL; }
    if (m_glowFullRT)  { m_glowFullRT->Release();  m_glowFullRT = NULL; }
    if (m_glowSmallRT) { m_glowSmallRT->Release(); m_glowSmallRT = NULL; }
    for (int i = 0; i < 2; ++i)
        if (m_front[i]) { m_front[i]->Release(); m_front[i] = NULL; }
    if (m_tileDepth) { m_tileDepth->Release(); m_tileDepth = NULL; }
    if (m_tileRT)    { m_tileRT->Release();    m_tileRT = NULL; }
    if (m_device)    { m_device->Release();    m_device = NULL; }
    if (m_d3d)       { m_d3d->Release();       m_d3d = NULL; }
}

void XboxRenderer::SetClearColor(float r, float g, float b)
{
    m_clearR = r; m_clearG = g; m_clearB = b;
}

bool XboxRenderer::BeginFrame()
{
    if (!m_device)
        return false;

    m_device->SetRenderTarget(0, m_tileRT);
    m_device->SetDepthStencilSurface(m_tileDepth);

    D3DVIEWPORT9 vp;
    vp.X = 0; vp.Y = 0;
    vp.Width = (DWORD)m_width; vp.Height = (DWORD)m_height;
    vp.MinZ = 0.0f; vp.MaxZ = 1.0f;
    m_device->SetViewport(&vp);

    // Begin tiling: clears each tile to the background as it starts. Alpha
    // clears to ZERO — it is the emissive glow mask (see SetBloomShaders).
    D3DVECTOR4 clr = { m_clearR, m_clearG, m_clearB, 0.0f };
    m_device->BeginTiling(0, kTileCount, kTileRects, &clr, 1.0f, 0);
    return true;
}

void XboxRenderer::EndFrame()
{
    if (!m_device)
        return;

    m_device->SynchronizeToPresentationInterval();

    // Replay the recorded scene for each tile and resolve them into the front
    // buffer, clearing the EDRAM targets afterwards.
    D3DVECTOR4 clr = { m_clearR, m_clearG, m_clearB, 0.0f };
    m_device->EndTiling(D3DRESOLVE_RENDERTARGET0 | D3DRESOLVE_ALLFRAGMENTS |
                        D3DRESOLVE_CLEARRENDERTARGET | D3DRESOLVE_CLEARDEPTHSTENCIL,
                        NULL, m_front[m_frontIdx], &clr, 1.0f, 0, NULL);

    // Emissive glow: blur the mask the scene exported in the resolved frame's
    // alpha and add the halo back, re-resolving into the same front buffer.
    if (m_glowBrightPS && m_glowBlurPS && m_glowCombinePS &&
        m_glowSmallRT && m_glowFullRT && m_glowTexA && m_glowTexB)
        RenderGlow();

    m_device->Swap(m_front[m_frontIdx], NULL);
    m_frontIdx ^= 1;
}

void XboxRenderer::GlowPass(IDirect3DSurface9* dst, int dw, int dh,
                            IDirect3DVertexShader9* vs, IDirect3DPixelShader9* ps,
                            IDirect3DBaseTexture9* src)
{
    struct QuadVtx { float x, y, z, u, v; };
    static const QuadVtx quad[4] = {
        { -1.0f,  1.0f, 0.0f, 0.0f, 0.0f },
        {  1.0f,  1.0f, 0.0f, 1.0f, 0.0f },
        { -1.0f, -1.0f, 0.0f, 0.0f, 1.0f },
        {  1.0f, -1.0f, 0.0f, 1.0f, 1.0f },
    };
    m_device->SetRenderTarget(0, dst);
    D3DVIEWPORT9 vp;
    vp.X = 0; vp.Y = 0; vp.Width = (DWORD)dw; vp.Height = (DWORD)dh;
    vp.MinZ = 0.0f; vp.MaxZ = 1.0f;
    m_device->SetViewport(&vp);
    const float half[4] = { 1.0f / (float)dw, 1.0f / (float)dh, 0.0f, 0.0f };
    m_device->SetVertexShaderConstantF(0, half, 1);
    m_device->SetVertexShader(vs);
    m_device->SetPixelShader(ps);
    m_device->SetTexture(0, src);
    m_device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(QuadVtx));
}

void XboxRenderer::RenderGlow()
{
    IDirect3DTexture9* frame = m_front[m_frontIdx];

    m_device->SetDepthStencilSurface(NULL);
    m_device->SetRenderState(D3DRS_ZENABLE, FALSE);
    m_device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    m_device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    m_device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    m_device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    m_device->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
    m_device->SetRenderState(D3DRS_COLORWRITEENABLE,
        D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN |
        D3DCOLORWRITEENABLE_BLUE | D3DCOLORWRITEENABLE_ALPHA);
    m_device->SetFVF(D3DFVF_XYZ | D3DFVF_TEX1);
    m_device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    m_device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    m_device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    m_device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);

    const D3DRECT smallRect = { 0, 0, kGlowW, kGlowH };
    const D3DRECT fullRect  = { 0, 0, m_width, m_height };

    // 1) Glow extract: frame rgb * alpha mask, at quarter res.
    GlowPass(m_glowSmallRT, kGlowW, kGlowH, m_glowBrightVS, m_glowBrightPS, frame);
    m_device->Resolve(D3DRESOLVE_RENDERTARGET0, &smallRect, m_glowTexA, NULL, 0, 0, NULL, 0.0f, 0, NULL);

    // 2) Separable blur: horizontal, then vertical.
    const float stepH[4] = { kGlowStep / (float)kGlowW, 0.0f, 0.0f, 0.0f };
    m_device->SetPixelShaderConstantF(0, stepH, 1);
    GlowPass(m_glowSmallRT, kGlowW, kGlowH, m_glowBlurVS, m_glowBlurPS, m_glowTexA);
    m_device->Resolve(D3DRESOLVE_RENDERTARGET0, &smallRect, m_glowTexB, NULL, 0, 0, NULL, 0.0f, 0, NULL);

    const float stepV[4] = { 0.0f, kGlowStep / (float)kGlowH, 0.0f, 0.0f };
    m_device->SetPixelShaderConstantF(0, stepV, 1);
    GlowPass(m_glowSmallRT, kGlowW, kGlowH, m_glowBlurVS, m_glowBlurPS, m_glowTexB);
    m_device->Resolve(D3DRESOLVE_RENDERTARGET0, &smallRect, m_glowTexA, NULL, 0, 0, NULL, 0.0f, 0, NULL);

    // 3) Composite: copy the frame, add the halo, resolve back into the frame.
    const float gainCopy[4] = { 1.0f, 0.0f, 0.0f, 0.0f };
    m_device->SetPixelShaderConstantF(0, gainCopy, 1);
    GlowPass(m_glowFullRT, m_width, m_height, m_glowCombineVS, m_glowCombinePS, frame);

    const float gainGlow[4] = { kGlowGain, 0.0f, 0.0f, 0.0f };
    m_device->SetPixelShaderConstantF(0, gainGlow, 1);
    m_device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    m_device->SetRenderState(D3DRS_SRCBLEND,  D3DBLEND_ONE);
    m_device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
    GlowPass(m_glowFullRT, m_width, m_height, m_glowCombineVS, m_glowCombinePS, m_glowTexA);
    m_device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);

    m_device->Resolve(D3DRESOLVE_RENDERTARGET0, &fullRect, frame, NULL, 0, 0, NULL, 0.0f, 0, NULL);
    m_device->SetTexture(0, NULL);
}
