#include "render/Renderer.h"

bool Renderer::Init(HWND hwnd)
{
    m_d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!m_d3d)
    {
        m_lastDeviceError = D3DERR_NOTAVAILABLE;
        return false;
    }

    ZeroMemory(&m_pp, sizeof(m_pp));
    m_pp.Windowed               = TRUE;
    m_pp.SwapEffect             = D3DSWAPEFFECT_DISCARD;
    m_pp.BackBufferFormat       = D3DFMT_UNKNOWN; // match the window
    m_pp.EnableAutoDepthStencil = TRUE;
    m_pp.AutoDepthStencilFormat = D3DFMT_D16;
    m_pp.PresentationInterval   = D3DPRESENT_INTERVAL_ONE; // vsync

    m_lastDeviceError = m_d3d->CreateDevice(
        D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
        D3DCREATE_HARDWARE_VERTEXPROCESSING, &m_pp, &m_device);
    if (FAILED(m_lastDeviceError))
    {
        m_lastDeviceError = m_d3d->CreateDevice(
            D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
            D3DCREATE_SOFTWARE_VERTEXPROCESSING, &m_pp, &m_device);
        if (FAILED(m_lastDeviceError))
        {
            m_d3d->Release();
            m_d3d = nullptr;
            return false;
        }
    }
    return true;
}

void Renderer::Shutdown()
{
    if (m_device) { m_device->Release(); m_device = nullptr; }
    if (m_d3d)    { m_d3d->Release();    m_d3d    = nullptr; }
}

bool Renderer::ResetDevice()
{
    for (ResetHandler& h : m_resetHandlers)
        if (h.on_lost) h.on_lost();

    HRESULT hr = m_device->Reset(&m_pp);
    if (hr == D3DERR_INVALIDCALL)
        return false; // a default-pool resource wasn't released

    for (ResetHandler& h : m_resetHandlers)
        if (h.on_reset) h.on_reset();
    return true;
}

bool Renderer::BeginFrame()
{
    if (!m_device)
        return false;

    // Recover from a lost device (alt-tab, driver reset, power event, ...).
    if (m_deviceLost)
    {
        HRESULT hr = m_device->TestCooperativeLevel();
        if (hr == D3DERR_DEVICELOST)
            return false; // still lost; try again next frame
        if (hr == D3DERR_DEVICENOTRESET && !ResetDevice())
            return false;
        m_deviceLost = false;
    }

    // Apply a pending resize.
    if (m_resizeW != 0 && m_resizeH != 0)
    {
        m_pp.BackBufferWidth  = m_resizeW;
        m_pp.BackBufferHeight = m_resizeH;
        m_resizeW = m_resizeH = 0;
        ResetDevice();
    }

    return true;
}

bool Renderer::BeginBackbuffer()
{
    if (!m_device)
        return false;
    m_device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, m_clear, 1.0f, 0);
    return m_device->BeginScene() >= 0;
}

void Renderer::EndFrame()
{
    m_device->EndScene();
    if (m_device->Present(nullptr, nullptr, nullptr, nullptr) == D3DERR_DEVICELOST)
        m_deviceLost = true;
}

void Renderer::OnResize(UINT width, UINT height)
{
    m_resizeW = width;
    m_resizeH = height;
}

void Renderer::SetClearColor(float r, float g, float b, float a)
{
    auto to8 = [](float v) -> int
    {
        int i = (int)(v * 255.0f + 0.5f);
        return i < 0 ? 0 : (i > 255 ? 255 : i);
    };
    m_clear = D3DCOLOR_ARGB(to8(a), to8(r), to8(g), to8(b));
}
