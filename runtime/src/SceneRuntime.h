#pragma once

#include "Content.h"
#include "SceneData.h"

#include <xtl.h>

// The stripped-down runtime renderer: the console analogue of the editor's
// SceneRenderer, minus everything editor-only. There is no grid, no gizmo, no
// selection highlight, and no keyboard/mouse camera — just the scene's meshes
// and custom-shader objects drawn from a fixed camera, straight to the back
// buffer. It reuses the editor's binary formats (.mesh / .scene) and material
// (standard.hlsl), so a scene authored in the editor renders the same here.
class SceneRuntime
{
public:
    // contentRoot is the deployed game root, e.g. "game:\". Loads the standard
    // material, the startup scene, and builds shared geometry. Returns false if
    // the device objects can't be created.
    bool Init(IDirect3DDevice9* device, const std::string& contentRoot);
    void Shutdown();

    // Draw the whole scene for one frame (device scene already begun by the
    // caller). dt advances gTime for animated custom shaders.
    void Render(float dt);

private:
    struct DrawItem
    {
        std::string model_path;
        D3DMATRIX   world;
    };
    struct ShaderItem
    {
        std::string shader_path;
        std::string model_path;
        float pos[3];
        float rot[3];
        float scale[3];
    };

    void BuildDrawLists();
    void DrawMesh(RtMesh* gm, const D3DMATRIX& world, const D3DMATRIX& vp, RtShader* mat);
    void DrawShaderItem(const ShaderItem& item, RtShader& shader, const D3DMATRIX& viewProj);
    IDirect3DTexture9* SolidTexture(D3DCOLOR argb);
    bool BuildGeometry();

    IDirect3DDevice9*            m_device;
    std::string                  m_root;
    Content                      m_content;
    RtScene                      m_scene;

    IDirect3DVertexDeclaration9* m_mesh_decl;
    IDirect3DTexture9*           m_def_white;
    IDirect3DTexture9*           m_def_normal;
    IDirect3DTexture9*           m_def_black;

    // Unit quad + unit cube (mesh vertex layout) for standalone shaders.
    IDirect3DVertexBuffer9*      m_quad_vb;
    IDirect3DIndexBuffer9*       m_quad_ib;
    IDirect3DVertexBuffer9*      m_cube_vb;
    IDirect3DIndexBuffer9*       m_cube_ib;

    // Fixed orbit camera (the editor's default framing; no input on the console).
    float m_yaw;
    float m_pitch;
    float m_distance;
    float m_target[3];
    float m_eye[3];

    float m_time;

    std::vector<DrawItem>   m_draw_items;
    std::vector<ShaderItem> m_shader_items;
};
