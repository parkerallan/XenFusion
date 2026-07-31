#pragma once

#include <d3d9.h>

#include <functional>
#include <vector>

// Thin wrapper over the graphics device.
//
// On PC this is Direct3D 9. The point of keeping it behind a class is that the
// Xbox 360 uses a close cousin of this API (Direct3D 9 for Xbox 360 / "D3D9x"),
// so an eventual console backend can implement the same surface. Keep engine
// code talking to Renderer, not to IDirect3DDevice9 directly, and that port
// stays tractable.
class Renderer
{
public:
    bool Init(HWND hwnd);
    void Shutdown();

    // Device management for the frame: recovers a lost device and applies a
    // pending resize. Returns false when nothing should be drawn this frame
    // (device lost or minimized) — caller should skip the frame.
    bool BeginFrame();

    // Opens the back-buffer scene (Clear + BeginScene). Call after any
    // offscreen passes, right before drawing the UI. Returns false if the
    // scene could not begin.
    bool BeginBackbuffer();

    // Ends the back-buffer scene and presents.
    void EndFrame();

    // Deferred: the actual back-buffer reset happens at the next BeginFrame.
    void OnResize(UINT width, UINT height);

    void SetClearColor(float r, float g, float b, float a = 1.0f);

    IDirect3DDevice9* Device() const { return m_device; }
    HRESULT LastDeviceError() const { return m_lastDeviceError; }

    // Register callbacks invoked around every device Reset (resize / lost).
    // on_lost must release all D3DPOOL_DEFAULT resources; on_reset recreates
    // them (or leaves recreation lazy). Called in registration order.
    void AddResetHandler(std::function<void()> on_lost, std::function<void()> on_reset)
    {
        m_resetHandlers.push_back({std::move(on_lost), std::move(on_reset)});
    }

private:
    bool ResetDevice();

    struct ResetHandler
    {
        std::function<void()> on_lost;
        std::function<void()> on_reset;
    };

    std::vector<ResetHandler> m_resetHandlers;
    IDirect3D9*           m_d3d        = nullptr;
    IDirect3DDevice9*     m_device     = nullptr;
    D3DPRESENT_PARAMETERS m_pp         = {};
    bool                  m_deviceLost = false;
    D3DCOLOR              m_clear      = D3DCOLOR_ARGB(255, 24, 24, 32);
    UINT                  m_resizeW    = 0;
    UINT                  m_resizeH    = 0;
    HRESULT               m_lastDeviceError = S_OK;
};
