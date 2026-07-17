#include "SceneRuntime.h"
#include "Endian.h"
#include "RtMath.h"

#include <stdio.h>
#include <string.h>

using namespace rmath;

// ---------------------------------------------------------------------------
// Shared frame inputs (same registers / values as the editor's standard material)
// ---------------------------------------------------------------------------
namespace
{
    const float kPi = 3.14159265f;

    // MeshVertex layout: pos(3) normal(3) tangent(3) uv(2) = 11 floats, 44 bytes.
    void PutVertex(unsigned char* p,
                   float px, float py, float pz,
                   float nx, float ny, float nz,
                   float tx, float ty, float tz,
                   float u, float v)
    {
        float f[11] = { px, py, pz, nx, ny, nz, tx, ty, tz, u, v };
        memcpy(p, f, sizeof(f)); // native-endian, matching the converted mesh buffers
    }
}

bool SceneRuntime::Init(IDirect3DDevice9* device, const std::string& contentRoot)
{
    m_device = device;
    m_root   = contentRoot;

    m_mesh_decl = NULL;
    m_def_white = m_def_normal = m_def_black = NULL;
    m_quad_vb = NULL; m_quad_ib = NULL; m_cube_vb = NULL; m_cube_ib = NULL;

    // Fixed camera: the editor's default orbit framing (no input on the console).
    m_yaw = 0.6f; m_pitch = 0.5f; m_distance = 20.0f;
    m_target[0] = m_target[1] = m_target[2] = 0.0f;
    m_eye[0] = m_eye[1] = m_eye[2] = 0.0f;
    m_time = 0.0f;

    m_content.Init(device, contentRoot);

    // Streaming: open game.spak once and drive it through the residency cache.
    // Absent/failed open is fine — every lookup then misses and ResolveMesh falls
    // back to the raw loader.
    m_pak.Open(m_content.Resolve("game.spak").c_str());
    m_cache.Init(device, &m_pak); // default 128 MB residency budget

    // Vertex declaration matching MeshVertex (pos/normal/tangent/uv).
    const D3DVERTEXELEMENT9 elems[] = {
        {0,  0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        {0, 12, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_NORMAL,   0},
        {0, 24, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TANGENT,  0},
        {0, 36, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
        D3DDECL_END()
    };
    if (FAILED(m_device->CreateVertexDeclaration(elems, &m_mesh_decl)))
        return false;

    m_def_white  = SolidTexture(D3DCOLOR_ARGB(255, 255, 255, 255)); // diffuse
    m_def_normal = SolidTexture(D3DCOLOR_ARGB(255, 128, 128, 255)); // flat normal
    m_def_black  = SolidTexture(D3DCOLOR_ARGB(255, 0, 0, 0));       // no specular

    if (!BuildGeometry())
        return false;

    m_content.LoadStandard();

    // Load the startup scene: <root>\game.proj -> startupScene, else Main.scene.
    std::string startup = scenedata::ReadStartupScene(m_content.Resolve("game.proj"));
    if (startup.empty())
        startup = "scenes/Main.scene";
    if (!scenedata::LoadScene(m_content.Resolve(startup), m_scene))
        OutputDebugStringA("scene: failed to load startup scene\n");

    BuildDrawLists();
    return true;
}

// Streamed mesh first (game.spak via the async residency cache); fall back to the
// raw D3DX loader for anything not cooked into the pak. A mesh that's in the pak
// but still streaming in returns NULL and is skipped this frame (no stall) rather
// than falling back to a synchronous load. Both paths ship until the phase-5 cutover.
RtMesh* SceneRuntime::ResolveMesh(const std::string& relPath)
{
    bool inPak = false;
    RtMesh* m = m_cache.GetMesh(relPath, &inPak);
    if (m)     return m;                    // resident
    if (inPak) return NULL;                 // streaming in — skip this frame
    return m_content.GetMesh(relPath);      // not in the pak — raw fallback
}

void SceneRuntime::Shutdown()
{
    m_cache.Shutdown();
    m_pak.Close();
    if (m_cube_ib)   { m_cube_ib->Release();   m_cube_ib = NULL; }
    if (m_cube_vb)   { m_cube_vb->Release();   m_cube_vb = NULL; }
    if (m_quad_ib)   { m_quad_ib->Release();   m_quad_ib = NULL; }
    if (m_quad_vb)   { m_quad_vb->Release();   m_quad_vb = NULL; }
    if (m_def_black) { m_def_black->Release(); m_def_black = NULL; }
    if (m_def_normal){ m_def_normal->Release();m_def_normal = NULL; }
    if (m_def_white) { m_def_white->Release(); m_def_white = NULL; }
    if (m_mesh_decl) { m_mesh_decl->Release(); m_mesh_decl = NULL; }
    m_content.Shutdown();
    m_device = NULL;
}

IDirect3DTexture9* SceneRuntime::SolidTexture(D3DCOLOR argb)
{
    IDirect3DTexture9* t = NULL;
    if (FAILED(m_device->CreateTexture(1, 1, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t, NULL)))
        return NULL;
    D3DLOCKED_RECT r;
    if (SUCCEEDED(t->LockRect(0, &r, NULL, 0)))
    {
        *(D3DCOLOR*)r.pBits = argb;
        t->UnlockRect(0);
    }
    return t;
}

bool SceneRuntime::BuildGeometry()
{
    // Unit quad (XY plane, uv 0..1) — for "quad" custom shaders.
    const UINT quadVBytes = 4 * 44;
    if (FAILED(m_device->CreateVertexBuffer(quadVBytes, 0, 0, D3DPOOL_MANAGED, &m_quad_vb, NULL)))
        return false;
    {
        unsigned char* p = NULL;
        m_quad_vb->Lock(0, 0, (void**)&p, 0);
        PutVertex(p +   0, -0.5f, -0.5f, 0.0f,  0,0,-1,  1,0,0,  0.0f, 0.0f);
        PutVertex(p +  44,  0.5f, -0.5f, 0.0f,  0,0,-1,  1,0,0,  1.0f, 0.0f);
        PutVertex(p +  88,  0.5f,  0.5f, 0.0f,  0,0,-1,  1,0,0,  1.0f, 1.0f);
        PutVertex(p + 132, -0.5f,  0.5f, 0.0f,  0,0,-1,  1,0,0,  0.0f, 1.0f);
        m_quad_vb->Unlock();
    }
    if (FAILED(m_device->CreateIndexBuffer(6 * 4, 0, D3DFMT_INDEX32, D3DPOOL_MANAGED, &m_quad_ib, NULL)))
        return false;
    {
        unsigned int idx[6] = { 0, 1, 2, 0, 2, 3 };
        void* dst = NULL;
        m_quad_ib->Lock(0, 0, &dst, 0);
        memcpy(dst, idx, sizeof(idx)); // native-endian indices
        m_quad_ib->Unlock();
    }

    // Unit cube [-0.5,0.5]^3 — for "volume" (raymarched) custom shaders.
    const float c = 0.5f;
    const float cx[8] = {-c,  c,  c, -c, -c,  c,  c, -c};
    const float cy[8] = {-c, -c,  c,  c, -c, -c,  c,  c};
    const float cz[8] = {-c, -c, -c, -c,  c,  c,  c,  c};
    if (FAILED(m_device->CreateVertexBuffer(8 * 44, 0, 0, D3DPOOL_MANAGED, &m_cube_vb, NULL)))
        return false;
    {
        unsigned char* p = NULL;
        m_cube_vb->Lock(0, 0, (void**)&p, 0);
        for (int k = 0; k < 8; ++k)
            PutVertex(p + k * 44, cx[k], cy[k], cz[k],  0,0,0,  0,0,0,  0,0);
        m_cube_vb->Unlock();
    }
    if (FAILED(m_device->CreateIndexBuffer(36 * 4, 0, D3DFMT_INDEX32, D3DPOOL_MANAGED, &m_cube_ib, NULL)))
        return false;
    {
        unsigned int idx[36] = {
            0,1,2, 0,2,3,  5,4,7, 5,7,6,  4,0,3, 4,3,7,
            1,5,6, 1,6,2,  4,5,1, 4,1,0,  3,2,6, 3,6,7,
        };
        void* dst = NULL;
        m_cube_ib->Lock(0, 0, &dst, 0);
        memcpy(dst, idx, sizeof(idx)); // native-endian indices
        m_cube_ib->Unlock();
    }
    return true;
}

// Capture the models + standalone shaders to draw from the loaded scene. Static
// on the console (no editing), so this runs once at Init.
void SceneRuntime::BuildDrawLists()
{
    m_draw_items.clear();
    m_shader_items.clear();

    for (size_t i = 0; i < m_scene.objects.size(); ++i)
    {
        const RtObject& o = m_scene.objects[i];
        if (!o.visible)
            continue;

        std::string model_path, shader_path;
        for (size_t a = 0; a < o.attributes.size(); ++a)
        {
            const RtAttribute& at = o.attributes[a];
            if (at.type == "3D Model" && !at.model_path.empty() && model_path.empty())
                model_path = at.model_path;
            else if (at.type == "Shader" && !at.shader_path.empty() && shader_path.empty())
                shader_path = at.shader_path;
        }

        bool shader_owns_mesh = false;
        if (!shader_path.empty())
        {
            RtShader* cs = m_content.GetShader(shader_path);
            shader_owns_mesh = cs && cs->state.geometry == RtShaderState::Model;

            ShaderItem si;
            si.shader_path = shader_path;
            si.model_path  = model_path;
            for (int k = 0; k < 3; ++k) { si.pos[k] = o.position[k]; si.rot[k] = o.rotation[k]; si.scale[k] = o.scale[k]; }
            m_shader_items.push_back(si);
        }

        if (!model_path.empty() && !shader_owns_mesh)
        {
            DrawItem di;
            di.model_path = model_path;
            di.world = ComposeWorld(o.position, o.rotation, o.scale);
            m_draw_items.push_back(di);
        }
    }
}

void SceneRuntime::Render(float dt)
{
    if (!m_device)
        return;

    m_time += dt;

    // Register a budgeted batch of finished streaming loads into live resources
    // (the worker thread did the read + decompress off this thread).
    m_cache.Update(8);

    // Fixed camera (orbit params baked at Init; no input on the console).
    Vec3 dir = { cosf(m_pitch) * sinf(m_yaw), sinf(m_pitch), cosf(m_pitch) * cosf(m_yaw) };
    Vec3 at  = { m_target[0], m_target[1], m_target[2] };
    Vec3 eye = { at.x + dir.x * m_distance, at.y + dir.y * m_distance, at.z + dir.z * m_distance };
    m_eye[0] = eye.x; m_eye[1] = eye.y; m_eye[2] = eye.z;
    Vec3 up = { 0.0f, 1.0f, 0.0f };
    const D3DMATRIX view = LookAtLH(eye, at, up);
    const D3DMATRIX proj = PerspectiveFovLH(kPi / 4.0f, 16.0f / 9.0f, 0.5f, 100.0f);
    const D3DMATRIX vp   = Multiply(view, proj);

    Vec3 lraw = { -0.4f, -1.0f, -0.5f };
    Vec3 ln = Normalize(lraw);
    const float lightDir[4] = { ln.x, ln.y, ln.z, 0.0f };
    const float camPos[4]   = { m_eye[0], m_eye[1], m_eye[2], 0.0f };
    const float ambient[4]  = { 0.22f, 0.22f, 0.25f, 0.0f };

    m_device->SetVertexDeclaration(m_mesh_decl);

    // Common state (the 360 has no fixed-function pipeline; everything is shaded).
    // The 360 doesn't guarantee PC render-state defaults, so set them explicitly.
    m_device->SetRenderState(D3DRS_ZENABLE, TRUE);
    m_device->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
    m_device->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
    m_device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    m_device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    m_device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    m_device->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
    m_device->SetRenderState(D3DRS_COLORWRITEENABLE,
        D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN |
        D3DCOLORWRITEENABLE_BLUE | D3DCOLORWRITEENABLE_ALPHA);

    // --- Scene models (standard diffuse / normal / specular material) ---
    RtShader* std_mat = m_content.Standard();
    if (!m_draw_items.empty() && std_mat && std_mat->Valid())
    {
        m_device->SetVertexShader(std_mat->vs);
        m_device->SetPixelShader(std_mat->ps);
        for (DWORD s = 0; s < 3; ++s)
        {
            m_device->SetSamplerState(s, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
            m_device->SetSamplerState(s, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
            m_device->SetSamplerState(s, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
            m_device->SetSamplerState(s, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
        }
        m_device->SetPixelShaderConstantF(0, lightDir, 1);
        m_device->SetPixelShaderConstantF(1, camPos, 1);
        m_device->SetPixelShaderConstantF(2, ambient, 1);

        // Pass 1 — opaque + cutout subsets (both write depth). Cutout (hair cards,
        // foliage) is a masked alpha: the per-subset states (alpha test + Xenos
        // alpha-to-coverage) are set inside DrawMesh, per material subset.
        m_device->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
        m_device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        m_device->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATEREQUAL);
        for (size_t i = 0; i < m_draw_items.size(); ++i)
        {
            RtMesh* gm = ResolveMesh(m_draw_items[i].model_path);
            if (gm)
                DrawMesh(gm, m_draw_items[i].world, vp, std_mat, false /*opaque+cutout*/);
        }
        m_device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        m_device->SetRenderState(D3DRS_ALPHATOMASKENABLE, FALSE);

        // Pass 2 — alpha-blended (glass, water), after all opaque depth. Depth TEST
        // ON so opaque geometry in front correctly occludes it; depth WRITE OFF so
        // the transparent surface doesn't populate the depth buffer. Verified in
        // Xenia: the glass renders correctly and is occluded by nearer geometry (the
        // old ZENABLE-off workaround was compensating for a non-existent quirk).
        m_device->SetRenderState(D3DRS_ZENABLE, TRUE);
        m_device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        m_device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        m_device->SetRenderState(D3DRS_SRCBLEND,  D3DBLEND_SRCALPHA);
        m_device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        for (size_t i = 0; i < m_draw_items.size(); ++i)
        {
            RtMesh* gm = ResolveMesh(m_draw_items[i].model_path);
            if (gm)
                DrawMesh(gm, m_draw_items[i].world, vp, std_mat, true /*blend*/);
        }
        m_device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        m_device->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
    }

    // --- Standalone custom-shader objects ---
    if (!m_shader_items.empty())
    {
        m_device->SetVertexDeclaration(m_mesh_decl);
        for (size_t i = 0; i < m_shader_items.size(); ++i)
        {
            RtShader* cs = m_content.GetShader(m_shader_items[i].shader_path);
            if (cs && cs->Valid())
                DrawShaderItem(m_shader_items[i], *cs, vp);
        }
        // Restore defaults for next frame.
        m_device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        m_device->SetRenderState(D3DRS_ZENABLE, TRUE);
        m_device->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
        m_device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    }
}

// Draw one standard-material mesh's subsets for a pass: upload its transform
// (via the constant table so packing is correct), then per material subset bind
// its textures, set its cutout states (opaque pass only), and draw its index
// range. Pass-level states (blend / depth) are set by the caller.
void SceneRuntime::DrawMesh(RtMesh* gm, const D3DMATRIX& world, const D3DMATRIX& vp, RtShader* mat, bool blendPass)
{
    // Skip the transform upload when nothing in this mesh belongs to the pass.
    bool any = false;
    for (size_t i = 0; i < gm->subsets.size(); ++i)
        if ((gm->subsets[i].alpha == RtBlend) == blendPass) { any = true; break; }
    if (!any)
        return;

    const D3DMATRIX wvp = Multiply(world, vp);
    if (mat->vsConstants)
    {
        if (mat->hWVP)   mat->vsConstants->SetMatrix(m_device, mat->hWVP,   (const D3DXMATRIX*)&wvp);
        if (mat->hWorld) mat->vsConstants->SetMatrix(m_device, mat->hWorld, (const D3DXMATRIX*)&world);
    }
    m_device->SetStreamSource(0, gm->vb, 0, 44);
    m_device->SetIndices(gm->ib);

    for (size_t i = 0; i < gm->subsets.size(); ++i)
    {
        const RtSubset& s = gm->subsets[i];
        if ((s.alpha == RtBlend) != blendPass)
            continue;
        if (!blendPass)
        {
            if (s.alpha == RtCutout)
            {
                // Alpha test at the mask's mid-point drops the fully-transparent
                // background AND the faint low-alpha haze between strands (the pale
                // rim); alpha-to-coverage (Xenos native) anti-aliases the remaining
                // solid strand edges via MSAA coverage.
                m_device->SetRenderState(D3DRS_ALPHATESTENABLE, TRUE);
                m_device->SetRenderState(D3DRS_ALPHAREF, 170);
                m_device->SetRenderState(D3DRS_ALPHATOMASKENABLE, TRUE);
            }
            else
            {
                m_device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
                m_device->SetRenderState(D3DRS_ALPHATOMASKENABLE, FALSE);
            }
        }
        // Bump-offset strength (PS c5): max UV shift across the height field
        // packed in a normal map's alpha (keep in sync with the editor's
        // SceneRenderer.cpp). Subsets without a height field upload exactly 0,
        // so their UVs — and the cutout alpha they feed — are untouched.
        const float bump[4] = { s.normalHasHeight ? 0.08f : 0.0f, 0.0f, 0.0f, 0.0f };
        m_device->SetPixelShaderConstantF(5, bump, 1);
        m_device->SetTexture(0, s.diffuse  ? s.diffuse  : m_def_white);
        m_device->SetTexture(1, s.normal   ? s.normal   : m_def_normal);
        m_device->SetTexture(2, s.specular ? s.specular : m_def_black);
        m_device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, gm->vertexCount,
                                       s.indexStart, s.indexCount / 3);
    }
}

// Draw one standalone shader — geometry, transform, uniforms, parsed render
// states, then the draw. Mirrors the editor's SceneRenderer::DrawShaderItem.
void SceneRuntime::DrawShaderItem(const ShaderItem& item, RtShader& shader, const D3DMATRIX& viewProj)
{
    const RtShaderState& s = shader.state;
    const float d2r = kPi / 180.0f;

    const D3DMATRIX rot = Multiply(Multiply(RotationX(item.rot[0] * d2r),
                                            RotationY(item.rot[1] * d2r)),
                                   RotationZ(item.rot[2] * d2r));
    D3DMATRIX w = Multiply(Multiply(Scaling(item.scale[0], item.scale[1], item.scale[2]), rot),
                           Translation(item.pos[0], item.pos[1], item.pos[2]));

    IDirect3DVertexBuffer9* vb = m_quad_vb;
    IDirect3DIndexBuffer9*  ib = m_quad_ib;
    UINT verts = 4, prims = 2;

    if (s.geometry == RtShaderState::Volume && m_cube_vb && m_cube_ib)
    {
        vb = m_cube_vb; ib = m_cube_ib; verts = 8; prims = 12;
        // Camera in the cube's local space, so the pixel shader can raymarch it.
        const D3DMATRIX invW =
            Multiply(Multiply(Translation(-item.pos[0], -item.pos[1], -item.pos[2]), Transpose3(rot)),
                     Scaling(1.0f / item.scale[0], 1.0f / item.scale[1], 1.0f / item.scale[2]));
        const Vec3 e = { m_eye[0], m_eye[1], m_eye[2] };
        const Vec3 camObj = TransformPoint(e, invW);
        const float camObj4[4] = { camObj.x, camObj.y, camObj.z, 0.0f };
        m_device->SetPixelShaderConstantF(4, camObj4, 1); // gCamObj (PS c4)
    }
    RtMesh* model_gm = NULL; // "model" geometry: drawn per material subset
    if (s.geometry == RtShaderState::Model)
    {
        model_gm = ResolveMesh(item.model_path);
        if (!model_gm)
            return;
        vb = model_gm->vb; ib = model_gm->ib;
        verts = model_gm->vertexCount; prims = model_gm->indexCount / 3;
        for (DWORD t = 0; t < 3; ++t)
        {
            m_device->SetSamplerState(t, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
            m_device->SetSamplerState(t, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
            m_device->SetSamplerState(t, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
            m_device->SetSamplerState(t, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
        }
    }

    // Parsed render states — flat, no switching.
    m_device->SetRenderState(D3DRS_ALPHABLENDENABLE, s.alphaBlend);
    m_device->SetRenderState(D3DRS_SRCBLEND,  s.srcBlend);
    m_device->SetRenderState(D3DRS_DESTBLEND, s.destBlend);
    m_device->SetRenderState(D3DRS_ZENABLE,      s.zEnable);
    m_device->SetRenderState(D3DRS_ZWRITEENABLE, s.zWrite);
    m_device->SetRenderState(D3DRS_CULLMODE,     s.cull);

    Vec3 lraw = { -0.4f, -1.0f, -0.5f };
    Vec3 ln = Normalize(lraw);
    const float lightDir[4] = { ln.x, ln.y, ln.z, 0.0f };
    const float camPos[4]   = { m_eye[0], m_eye[1], m_eye[2], 0.0f };
    const float ambient[4]  = { 0.22f, 0.22f, 0.25f, 0.0f };
    const float time4[4]    = { m_time, 0.0f, 0.0f, 0.0f };

    const D3DMATRIX wvp = Multiply(w, viewProj);
    m_device->SetVertexShader(shader.vs);
    m_device->SetPixelShader(shader.ps);
    if (shader.vsConstants)
    {
        // Custom shaders may declare only gWVP (e.g. Fire.hlsl has no gWorld); the
        // handle is 0 when absent, and SetMatrix with a null handle faults on the 360.
        if (shader.hWVP)   shader.vsConstants->SetMatrix(m_device, shader.hWVP,   (const D3DXMATRIX*)&wvp);
        if (shader.hWorld) shader.vsConstants->SetMatrix(m_device, shader.hWorld, (const D3DXMATRIX*)&w);
    }
    m_device->SetPixelShaderConstantF(0, lightDir, 1);
    m_device->SetPixelShaderConstantF(1, camPos, 1);
    m_device->SetPixelShaderConstantF(2, ambient, 1);
    m_device->SetPixelShaderConstantF(3, time4, 1); // gTime
    m_device->SetStreamSource(0, vb, 0, 44);
    m_device->SetIndices(ib);
    if (model_gm)
    {
        // "model" geometry: one draw per material subset, its textures bound.
        for (size_t i = 0; i < model_gm->subsets.size(); ++i)
        {
            const RtSubset& sub = model_gm->subsets[i];
            m_device->SetTexture(0, sub.diffuse  ? sub.diffuse  : m_def_white);
            m_device->SetTexture(1, sub.normal   ? sub.normal   : m_def_normal);
            m_device->SetTexture(2, sub.specular ? sub.specular : m_def_black);
            m_device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, verts,
                                           sub.indexStart, sub.indexCount / 3);
        }
    }
    else
        m_device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, verts, 0, prims);
}
