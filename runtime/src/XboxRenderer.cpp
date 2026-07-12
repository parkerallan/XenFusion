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
}

XboxRenderer::XboxRenderer()
    : m_d3d(NULL), m_device(NULL), m_tileRT(NULL), m_tileDepth(NULL),
      m_frontIdx(0), m_clearR(0.094f), m_clearG(0.094f), m_clearB(0.106f),
      m_width(1280), m_height(720)
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
    // scans out; the tiles resolve into these.
    for (int i = 0; i < 2; ++i)
    {
        if (FAILED(m_device->CreateTexture(m_width, m_height, 1, 0,
                                           (D3DFORMAT)MAKESRGBFMT(D3DFMT_LE_X8R8G8B8),
                                           D3DPOOL_DEFAULT, &m_front[i], NULL)))
            return false;
    }
    m_frontIdx = 0;
    return true;
}

void XboxRenderer::Shutdown()
{
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

    // Begin tiling: clears each tile to the background as it starts.
    D3DVECTOR4 clr = { m_clearR, m_clearG, m_clearB, 1.0f };
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
    D3DVECTOR4 clr = { m_clearR, m_clearG, m_clearB, 1.0f };
    m_device->EndTiling(D3DRESOLVE_RENDERTARGET0 | D3DRESOLVE_ALLFRAGMENTS |
                        D3DRESOLVE_CLEARRENDERTARGET | D3DRESOLVE_CLEARDEPTHSTENCIL,
                        NULL, m_front[m_frontIdx], &clr, 1.0f, 0, NULL);

    m_device->Swap(m_front[m_frontIdx], NULL);
    m_frontIdx ^= 1;
}
