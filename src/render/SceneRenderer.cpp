#include "render/SceneRenderer.h"

#include "state/EngineState.h"

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

// ---------------------------------------------------------------------------
// Minimal matrix helpers (left-handed, row-major to match D3D9). Hand-rolled
// so we don't pull in the legacy d3dx9 helper library.
// ---------------------------------------------------------------------------
namespace
{
    struct Vec3 { float x, y, z; };

    Vec3 Sub(const Vec3& a, const Vec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
    float Dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
    Vec3 Cross(const Vec3& a, const Vec3& b)
    {
        return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
    }
    Vec3 Normalize(const Vec3& v)
    {
        float len = std::sqrt(Dot(v, v));
        if (len < 1e-6f) return {0, 0, 0};
        return {v.x / len, v.y / len, v.z / len};
    }

    D3DMATRIX LookAtLH(const Vec3& eye, const Vec3& at, const Vec3& up)
    {
        Vec3 z = Normalize(Sub(at, eye));
        Vec3 x = Normalize(Cross(up, z));
        Vec3 y = Cross(z, x);

        D3DMATRIX m = {};
        m._11 = x.x; m._12 = y.x; m._13 = z.x; m._14 = 0.0f;
        m._21 = x.y; m._22 = y.y; m._23 = z.y; m._24 = 0.0f;
        m._31 = x.z; m._32 = y.z; m._33 = z.z; m._34 = 0.0f;
        m._41 = -Dot(x, eye); m._42 = -Dot(y, eye); m._43 = -Dot(z, eye); m._44 = 1.0f;
        return m;
    }

    D3DMATRIX PerspectiveFovLH(float fovY, float aspect, float zn, float zf)
    {
        float yScale = 1.0f / std::tan(fovY * 0.5f);
        float xScale = yScale / aspect;

        D3DMATRIX m = {};
        m._11 = xScale;
        m._22 = yScale;
        m._33 = zf / (zf - zn);
        m._34 = 1.0f;
        m._43 = -zn * zf / (zf - zn);
        return m;
    }

    D3DMATRIX Identity()
    {
        D3DMATRIX m = {};
        m._11 = m._22 = m._33 = m._44 = 1.0f;
        return m;
    }
}

void SceneRenderer::Initialize(IDirect3DDevice9* device)
{
    m_device = device;
    BuildGrid();
}

void SceneRenderer::Shutdown()
{
    OnDeviceLost();
    m_device = nullptr;
}

void SceneRenderer::OnDeviceLost()
{
    if (m_depth)     { m_depth->Release();     m_depth = nullptr; }
    if (m_rtSurface) { m_rtSurface->Release(); m_rtSurface = nullptr; }
    if (m_rt)        { m_rt->Release();        m_rt = nullptr; }
    m_width = m_height = 0;
}

void SceneRenderer::BeginFrame()
{
    m_renderRequested = false;
}

// --- UI phase: runs inside the ImGui frame -------------------------------
void SceneRenderer::RenderUi(EngineState& state)
{
    const ImVec2 avail = ImGui::GetContentRegionAvail();

    // No project -> nothing to render; show an empty viewport.
    if (!state.HasProject())
    {
        const ImVec2 q0 = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##noproj", ImVec2((std::max)(avail.x, 1.0f), (std::max)(avail.y, 1.0f)));
        const char* msg = "No project open";
        const ImVec2 ts = ImGui::CalcTextSize(msg);
        ImGui::GetWindowDrawList()->AddText(
            ImVec2(q0.x + (avail.x - ts.x) * 0.5f, q0.y + (avail.y - ts.y) * 0.5f),
            IM_COL32(130, 130, 130, 255), msg);
        return; // m_renderRequested stays false -> RenderGpu no-ops
    }

    const int w = (int)avail.x;
    const int h = (int)avail.y;

    // Size the target now, while the panel size is known, so the texture id
    // recorded below is valid and correctly sized for this frame.
    const bool ready = EnsureTarget(w, h);

    // Capture state for the GPU pass.
    auto to8 = [](float v) { int i = (int)(v * 255.0f + 0.5f); return i < 0 ? 0 : (i > 255 ? 255 : i); };
    m_background = D3DCOLOR_ARGB(255, to8(state.clear_color[0]),
                                 to8(state.clear_color[1]), to8(state.clear_color[2]));

    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    if (ready && m_rt)
    {
        ImGui::Image((ImTextureID)(intptr_t)m_rt, avail);
        m_renderRequested = true; // tell RenderGpu to fill it this frame
        HandleCameraInput();      // must follow the Image item (uses IsItemHovered)
    }
    else
    {
        ImGui::InvisibleButton("##viewport_surface",
                               ImVec2((std::max)(avail.x, 1.0f), (std::max)(avail.y, 1.0f)));
        ImGui::GetWindowDrawList()->AddText(ImVec2(p0.x + 12.0f, p0.y + 12.0f),
                                            IM_COL32(140, 150, 130, 255), "Preparing scene...");
    }
}

// Orbit camera controls: right-drag rotates, middle-drag pans, wheel zooms.
void SceneRenderer::HandleCameraInput()
{
    ImGuiIO& io = ImGui::GetIO();
    const bool hovered = ImGui::IsItemHovered();

    // Start a drag only when it begins over the viewport; keep tracking it
    // (even off the image) until the button is released.
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right))  m_rotating = true;
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) m_panning  = true;
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Right))  m_rotating = false;
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Middle)) m_panning  = false;

    if (m_rotating)
    {
        m_yaw   -= io.MouseDelta.x * 0.01f;
        m_pitch -= io.MouseDelta.y * 0.01f;
        const float lim = 1.55f; // avoid flipping over the poles
        m_pitch = std::clamp(m_pitch, -lim, lim);
    }

    if (m_panning && (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f))
    {
        // Move the target across the camera's right / up plane so the scene
        // follows the cursor ("grab and drag").
        Vec3 dir     = { std::cos(m_pitch) * std::sin(m_yaw), std::sin(m_pitch),
                         std::cos(m_pitch) * std::cos(m_yaw) };
        Vec3 forward = { -dir.x, -dir.y, -dir.z };
        Vec3 right   = Normalize(Cross(Vec3{0, 1, 0}, forward));
        Vec3 camUp   = Cross(forward, right);

        const float s = m_distance * 0.0015f;
        m_target[0] += (right.x * -io.MouseDelta.x + camUp.x * io.MouseDelta.y) * s;
        m_target[1] += (right.y * -io.MouseDelta.x + camUp.y * io.MouseDelta.y) * s;
        m_target[2] += (right.z * -io.MouseDelta.x + camUp.z * io.MouseDelta.y) * s;
    }

    if (hovered && io.MouseWheel != 0.0f)
    {
        m_distance *= std::pow(0.9f, io.MouseWheel);
        m_distance = std::clamp(m_distance, 2.0f, 200.0f);
    }
}

// --- GPU phase: runs after ImGui::Render(), before the back-buffer scene ---
void SceneRenderer::RenderGpu(float dt)
{
    if (!m_device || !m_renderRequested || !m_rtSurface)
        return;

    (void)dt; // camera is fully input-driven

    // Redirect rendering to the offscreen target, remembering the back buffer.
    IDirect3DSurface9* prevRt    = nullptr;
    IDirect3DSurface9* prevDepth = nullptr;
    m_device->GetRenderTarget(0, &prevRt);
    m_device->GetDepthStencilSurface(&prevDepth);

    m_device->SetRenderTarget(0, m_rtSurface);
    m_device->SetDepthStencilSurface(m_depth);
    m_device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, m_background, 1.0f, 0);

    if (SUCCEEDED(m_device->BeginScene()))
    {
        // Orbit camera: eye sits on a sphere around the (pannable) target.
        Vec3 dir = { std::cos(m_pitch) * std::sin(m_yaw), std::sin(m_pitch),
                     std::cos(m_pitch) * std::cos(m_yaw) };
        Vec3 at  = { m_target[0], m_target[1], m_target[2] };
        Vec3 eye = { at.x + dir.x * m_distance, at.y + dir.y * m_distance,
                     at.z + dir.z * m_distance };
        Vec3 up  = { 0.0f, 1.0f, 0.0f };

        const float aspect = (float)m_width / (float)m_height;
        D3DMATRIX world = Identity();
        D3DMATRIX view  = LookAtLH(eye, at, up);
        D3DMATRIX proj  = PerspectiveFovLH(3.14159265f / 4.0f, aspect, 0.1f, 200.0f);

        m_device->SetTransform(D3DTS_WORLD, &world);
        m_device->SetTransform(D3DTS_VIEW, &view);
        m_device->SetTransform(D3DTS_PROJECTION, &proj);

        m_device->SetRenderState(D3DRS_LIGHTING, FALSE);
        m_device->SetRenderState(D3DRS_ZENABLE, TRUE);
        m_device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        m_device->SetTexture(0, nullptr);
        m_device->SetFVF(D3DFVF_XYZ | D3DFVF_DIFFUSE);

        if (!m_grid.empty())
        {
            m_device->DrawPrimitiveUP(D3DPT_LINELIST, (UINT)(m_grid.size() / 2),
                                      m_grid.data(), sizeof(Vertex));
        }

        m_device->EndScene();
    }

    // Restore the back buffer as the active target.
    m_device->SetRenderTarget(0, prevRt);
    m_device->SetDepthStencilSurface(prevDepth);
    if (prevRt)    prevRt->Release();
    if (prevDepth) prevDepth->Release();
}

void SceneRenderer::BuildGrid()
{
    m_grid.clear();

    const int     half  = 10;
    const float   extent = (float)half;
    const D3DCOLOR line  = D3DCOLOR_ARGB(255, 60, 60, 70);
    const D3DCOLOR xAxis = D3DCOLOR_ARGB(255, 170, 70, 70);  // along +X (z = 0)
    const D3DCOLOR zAxis = D3DCOLOR_ARGB(255, 70, 90, 170);  // along +Z (x = 0)
    const D3DCOLOR yAxis = D3DCOLOR_ARGB(255, 90, 170, 80);  // up

    for (int i = -half; i <= half; ++i)
    {
        const float f = (float)i;
        D3DCOLOR c = (i == 0) ? xAxis : line;
        m_grid.push_back({-extent, 0.0f, f, c});
        m_grid.push_back({ extent, 0.0f, f, c});
        c = (i == 0) ? zAxis : line;
        m_grid.push_back({f, 0.0f, -extent, c});
        m_grid.push_back({f, 0.0f,  extent, c});
    }

    m_grid.push_back({0.0f, 0.0f, 0.0f, yAxis});
    m_grid.push_back({0.0f, extent * 0.5f, 0.0f, yAxis});
}

bool SceneRenderer::EnsureTarget(int width, int height)
{
    if (width <= 0 || height <= 0)
        return false;
    if (m_rt && width == m_width && height == m_height)
        return true;

    OnDeviceLost(); // release any existing target before recreating

    if (FAILED(m_device->CreateTexture(width, height, 1, D3DUSAGE_RENDERTARGET,
                                       D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &m_rt, nullptr)))
        return false;

    if (FAILED(m_rt->GetSurfaceLevel(0, &m_rtSurface)))
    {
        OnDeviceLost();
        return false;
    }

    if (FAILED(m_device->CreateDepthStencilSurface(width, height, D3DFMT_D16,
                                                   D3DMULTISAMPLE_NONE, 0, TRUE, &m_depth, nullptr)))
    {
        OnDeviceLost();
        return false;
    }

    m_width  = width;
    m_height = height;
    return true;
}
