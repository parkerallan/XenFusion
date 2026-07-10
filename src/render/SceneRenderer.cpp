#include "render/SceneRenderer.h"

#include "project/ProjectIO.h"
#include "state/EngineState.h"

#include "imgui.h"
#include "ImGuizmo.h"

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

    D3DMATRIX Multiply(const D3DMATRIX& a, const D3DMATRIX& b)
    {
        D3DMATRIX r;
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j)
                r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] +
                            a.m[i][2] * b.m[2][j] + a.m[i][3] * b.m[3][j];
        return r;
    }

    D3DMATRIX Translation(float x, float y, float z)
    {
        D3DMATRIX m = Identity();
        m._41 = x; m._42 = y; m._43 = z;
        return m;
    }

    D3DMATRIX Scaling(float x, float y, float z)
    {
        D3DMATRIX m = Identity();
        m._11 = x; m._22 = y; m._33 = z;
        return m;
    }

    D3DMATRIX RotationX(float a)
    {
        D3DMATRIX m = Identity();
        const float c = std::cos(a), s = std::sin(a);
        m._22 = c; m._23 = s; m._32 = -s; m._33 = c;
        return m;
    }
    D3DMATRIX RotationY(float a)
    {
        D3DMATRIX m = Identity();
        const float c = std::cos(a), s = std::sin(a);
        m._11 = c; m._13 = -s; m._31 = s; m._33 = c;
        return m;
    }
    D3DMATRIX RotationZ(float a)
    {
        D3DMATRIX m = Identity();
        const float c = std::cos(a), s = std::sin(a);
        m._11 = c; m._12 = s; m._21 = -s; m._22 = c;
        return m;
    }

    // World = Scale * Rot(x,y,z) * Translation (row-vector convention).
    D3DMATRIX ComposeWorld(const SceneObject& o)
    {
        const float d2r = 3.14159265f / 180.0f;
        const D3DMATRIX rot = Multiply(Multiply(RotationX(o.rotation[0] * d2r),
                                                RotationY(o.rotation[1] * d2r)),
                                       RotationZ(o.rotation[2] * d2r));
        return Multiply(Multiply(Scaling(o.scale[0], o.scale[1], o.scale[2]), rot),
                        Translation(o.position[0], o.position[1], o.position[2]));
    }
}

void SceneRenderer::Initialize(IDirect3DDevice9* device)
{
    m_device = device;
    m_meshes.Init(device);
    BuildGrid();
}

void SceneRenderer::Shutdown()
{
    m_meshes.Shutdown();
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
    ImGuizmo::BeginFrame();

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

    m_meshes.SetProjectRoot(state.project_root); // clears cache on project change

    // Capture the models to draw from the selected scene.
    m_draw_items.clear();
    if (SceneFile* scene = state.SelectedScene())
    {
        for (int i = 0; i < (int)scene->objects.size(); ++i)
        {
            const SceneObject& o = scene->objects[i];
            if (!o.visible)
                continue;
            const bool selected = (i == state.selected_object);
            for (const ObjectAttribute& a : o.attributes)
            {
                if (a.type == "3D Model" && !a.model_path.empty())
                    m_draw_items.push_back({a.model_path, ComposeWorld(o), selected});
            }
        }
    }

    // Focus-key ("F") target: the selected object.
    m_has_focus = false;
    if (SceneObject* sel = state.SelectedObject())
    {
        m_focus_pos[0] = sel->position[0];
        m_focus_pos[1] = sel->position[1];
        m_focus_pos[2] = sel->position[2];
        m_focus_scale  = (std::max)({sel->scale[0], sel->scale[1], sel->scale[2]});
        m_has_focus = true;
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

        // Camera matrices (also used by RenderGpu, so they stay in sync).
        Vec3 cdir = { std::cos(m_pitch) * std::sin(m_yaw), std::sin(m_pitch),
                      std::cos(m_pitch) * std::cos(m_yaw) };
        Vec3 cat  = { m_target[0], m_target[1], m_target[2] };
        Vec3 ceye = { cat.x + cdir.x * m_distance, cat.y + cdir.y * m_distance,
                      cat.z + cdir.z * m_distance };
        m_view = LookAtLH(ceye, cat, {0.0f, 1.0f, 0.0f});
        m_proj = PerspectiveFovLH(3.14159265f / 4.0f, (float)m_width / (float)m_height, 0.1f, 200.0f);

        // Translation gizmo for the selected object.
        if (SceneObject* sel = state.SelectedObject())
        {
            ImGuizmo::SetOrthographic(false);
            ImGuizmo::SetDrawlist();
            ImGuizmo::SetRect(p0.x, p0.y, avail.x, avail.y);
            D3DMATRIX model = ComposeWorld(*sel);
            ImGuizmo::Manipulate(&m_view.m[0][0], &m_proj.m[0][0],
                                 ImGuizmo::TRANSLATE, ImGuizmo::WORLD, &model.m[0][0]);
            if (ImGuizmo::IsUsing())
            {
                sel->position[0] = model.m[3][0];
                sel->position[1] = model.m[3][1];
                sel->position[2] = model.m[3][2];
                if (SceneFile* s = state.SelectedScene()) s->dirty = true;
                m_gizmo_editing = true;
            }
            else if (m_gizmo_editing)
            {
                if (SceneFile* s = state.SelectedScene()) project::SaveScene(*s);
                m_gizmo_editing = false;
            }
        }
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

    // Focus key: frame the selected object (Unity-style "F").
    if (hovered && m_has_focus && ImGui::IsKeyPressed(ImGuiKey_F))
    {
        m_target[0] = m_focus_pos[0];
        m_target[1] = m_focus_pos[1];
        m_target[2] = m_focus_pos[2];
        m_distance  = std::clamp(m_focus_scale * 3.0f + 3.0f, 3.0f, 100.0f);
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
        // Camera matrices were computed in RenderUi (shared with the gizmo).
        D3DMATRIX world = Identity();
        m_device->SetTransform(D3DTS_WORLD, &world);
        m_device->SetTransform(D3DTS_VIEW, &m_view);
        m_device->SetTransform(D3DTS_PROJECTION, &m_proj);

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

        // --- Scene models ---
        if (!m_draw_items.empty())
        {
            // Simple fixed-function directional lighting (no PBR / materials).
            D3DLIGHT9 light = {};
            light.Type = D3DLIGHT_DIRECTIONAL;
            light.Diffuse.r = light.Diffuse.g = light.Diffuse.b = light.Diffuse.a = 1.0f;
            Vec3 ld = Normalize({-0.4f, -1.0f, -0.5f});
            light.Direction = {ld.x, ld.y, ld.z};
            m_device->SetLight(0, &light);
            m_device->LightEnable(0, TRUE);
            m_device->SetRenderState(D3DRS_LIGHTING, TRUE);
            m_device->SetRenderState(D3DRS_NORMALIZENORMALS, TRUE);
            m_device->SetRenderState(D3DRS_AMBIENT, D3DCOLOR_ARGB(255, 55, 55, 60));

            D3DMATERIAL9 mtl = {};
            mtl.Diffuse.r = mtl.Diffuse.g = mtl.Diffuse.b = 0.85f; mtl.Diffuse.a = 1.0f;
            mtl.Ambient = mtl.Diffuse;
            m_device->SetMaterial(&mtl);
            m_device->SetFVF(MESH_FVF);
            m_device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
            m_device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
            m_device->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);

            for (const DrawItem& item : m_draw_items)
            {
                GpuMesh* gm = m_meshes.Get(item.model_path);
                if (!gm)
                    continue;

                // Modulate the diffuse texture with the lit vertex color; if the
                // mesh has no texture, just use the lit color.
                if (gm->diffuse)
                {
                    m_device->SetTexture(0, gm->diffuse);
                    m_device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
                    m_device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
                    m_device->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
                }
                else
                {
                    m_device->SetTexture(0, nullptr);
                    m_device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
                    m_device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
                }

                m_device->SetTransform(D3DTS_WORLD, &item.world);
                m_device->SetStreamSource(0, gm->vb, 0, sizeof(MeshVertex));
                m_device->SetIndices(gm->ib);
                m_device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0,
                                               gm->vertexCount, 0, gm->indexCount / 3);
            }

            // Selection outline: redraw selected meshes as an orange wireframe.
            m_device->SetRenderState(D3DRS_LIGHTING, FALSE);
            m_device->SetRenderState(D3DRS_FILLMODE, D3DFILL_WIREFRAME);
            m_device->SetTexture(0, nullptr);
            m_device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
            m_device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TFACTOR);
            m_device->SetRenderState(D3DRS_TEXTUREFACTOR, D3DCOLOR_ARGB(255, 230, 138, 40));
            for (const DrawItem& item : m_draw_items)
            {
                if (!item.selected)
                    continue;
                GpuMesh* gm = m_meshes.Get(item.model_path);
                if (!gm)
                    continue;
                m_device->SetTransform(D3DTS_WORLD, &item.world);
                m_device->SetStreamSource(0, gm->vb, 0, sizeof(MeshVertex));
                m_device->SetIndices(gm->ib);
                m_device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0,
                                               gm->vertexCount, 0, gm->indexCount / 3);
            }

            // Restore states.
            m_device->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
            m_device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
            m_device->SetRenderState(D3DRS_LIGHTING, FALSE);
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
