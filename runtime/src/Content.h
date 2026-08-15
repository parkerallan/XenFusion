#pragma once

#include "anim/FaceDeform.h"

#include <xtl.h>
#include <d3dx9.h>

#include <map>
#include <string.h>
#include <string>
#include <vector>

// How a subset's diffuse alpha is used (mirrors the editor's AlphaKind),
// classified from the texture's pixels (at cook for the pak, at load for loose
// files).
enum RtAlphaKind { RtOpaque, RtCutout, RtBlend };

// One material's range within a mesh: draw indices [indexStart, indexStart +
// indexCount) with its own textures and alpha mode. Texture ownership follows
// the mesh's loader: Content-loaded meshes own them (Release() frees them);
// StreamCache meshes borrow them from the texture cache (never freed here).
struct RtSubset
{
    unsigned int       indexStart;
    unsigned int       indexCount;
    RtAlphaKind        alpha;
    bool               normalHasHeight; // normal map's alpha carries a height
                                        // field (0.5 = neutral) — bump offset
    IDirect3DTexture9* diffuse;
    IDirect3DTexture9* normal;
    IDirect3DTexture9* specular;
    IDirect3DTexture9* emissive; // NULL = no glow (black default at draw)
    IDirect3DTexture9* metallic; // NULL = dielectric (black default at draw)
    IDirect3DTexture9* clearcoat; // NULL = no lacquer (black default at draw)
    IDirect3DTexture9* roughness; // NULL = glossy (black default at draw)

    RtSubset() : indexStart(0), indexCount(0), alpha(RtOpaque), normalHasHeight(false),
                 diffuse(NULL), normal(NULL), specular(NULL), emissive(NULL), metallic(NULL),
                 clearcoat(NULL), roughness(NULL) {}
};

struct RtJoint
{
    unsigned int nameHash;
    int          parent;
    float        inverseBind[16];
    float        bindLocal[16];

    RtJoint() : nameHash(0), parent(-1)
    {
        memset(inverseBind, 0, sizeof(inverseBind));
        memset(bindLocal, 0, sizeof(bindLocal));
    }
};

// Mirrors the editor's MeshMorphTarget; `shape` is its ARKit index.
struct RtMorphTarget
{
    std::vector<face::MorphDelta> deltas;
    float                         positionScale;
    int                           shape;

    RtMorphTarget() : positionScale(0.0f), shape(face::kShapeNone) {}
};

// Baking permuted every blendshape-driven vertex into the contiguous range
// [firstVertex, firstVertex + vertexCount).
struct RtMorph
{
    std::vector<RtMorphTarget> targets;
    unsigned int               firstVertex;
    unsigned int               vertexCount;

    RtMorph() : firstVertex(0), vertexCount(0) {}
    bool HasMorph() const { return !targets.empty() && vertexCount != 0; }
    void Clear() { targets.clear(); firstVertex = 0; vertexCount = 0; }
};

// A mesh resident in GPU memory. Vertex layout matches the editor's MeshVertex
// (pos/normal/tangent/uv, 44 bytes), drawn through a vertex declaration. One
// shared VB/IB; each material subset draws its own index range.
struct RtMesh
{
    IDirect3DVertexBuffer9* vb;
    IDirect3DVertexBuffer9* skinVb;
    IDirect3DIndexBuffer9*  ib;
    std::vector<RtSubset>   subsets;
    std::vector<RtJoint>    joints;
    RtMorph                 morph;
    // Undeformed vertices, native-endian, morph meshes only: deformation
    // accumulates onto the base pose and vertex memory is write-combined.
    std::vector<unsigned char> morphBase;
    unsigned int            skeletonFingerprint;
    unsigned int            vertexCount;
    unsigned int            indexCount;
    float                   boundsMin[3];
    float                   boundsMax[3];

    RtMesh() : vb(NULL), skinVb(NULL), ib(NULL), skeletonFingerprint(0),
               vertexCount(0), indexCount(0)
    {
        memset(boundsMin, 0, sizeof(boundsMin));
        memset(boundsMax, 0, sizeof(boundsMax));
    }
    bool IsSkinned() const { return skinVb != NULL && !joints.empty(); }
    bool HasMorph() const { return morph.HasMorph() && !morphBase.empty(); }
    void Release(); // frees buffers + subset textures (Content-owned meshes only)
};

// A custom shader's render setup, from its own //@ header directives (same
// grammar the editor's ShaderCache parses).
struct RtShaderState
{
    enum Geometry { Quad, Volume, Model } geometry;
    DWORD alphaBlend;
    DWORD srcBlend;
    DWORD destBlend;
    DWORD zEnable;
    DWORD zWrite;
    DWORD cull;

    RtShaderState()
        : geometry(Quad), alphaBlend(FALSE),
          srcBlend(D3DBLEND_SRCALPHA), destBlend(D3DBLEND_INVSRCALPHA),
          zEnable(TRUE), zWrite(TRUE), cull(D3DCULL_NONE) {}
};

struct RtShader
{
    IDirect3DVertexShader9* vs;
    IDirect3DVertexShader9* skinVs;
    IDirect3DVertexShader9* lightmapVs;
    IDirect3DPixelShader9*  ps;
    IDirect3DPixelShader9*  shadowPs;
    LPD3DXCONSTANTTABLE     vsConstants; // matrix upload, packing-correct via SetMatrix
    LPD3DXCONSTANTTABLE     skinVsConstants;
    D3DXHANDLE              hWVP;        // resolved handles for the two matrices
    D3DXHANDLE              hWorld;
    D3DXHANDLE              hSkinWVP;
    D3DXHANDLE              hSkinWorld;
    RtShaderState           state;

    RtShader() : vs(NULL), skinVs(NULL), lightmapVs(NULL), ps(NULL), shadowPs(NULL), vsConstants(NULL),
                 skinVsConstants(NULL), hWVP(0), hWorld(0), hSkinWVP(0), hSkinWorld(0) {}
    bool Valid() const { return vs && ps; }
    void Release();
};

// Loads and caches meshes and shaders from the deployed content root (game:\),
// mirroring the editor's MeshCache / ShaderCache but load-only and console-side:
// meshes come from shipped .mesh blobs and shaders are compiled from .hlsl at
// load with the XDK's D3DX (the PC .cso won't run on Xenos).
class Content
{
public:
    void Init(IDirect3DDevice9* device, const std::string& contentRoot);
    void Shutdown();

    // GpuMesh / shader for a scene-relative path (e.g. "assets/models/Fox.mesh").
    // Returns NULL on failure (logged once via OutputDebugString).
    RtMesh*   GetMesh(const std::string& relPath);
    RtShader* GetShader(const std::string& relPath);

    // Compile the built-in material from <root>/shaders/standard.hlsl.
    bool LoadStandard();
    RtShader* Standard() { return &m_standard; }

    // Load one built-in shader pair by stem (e.g. "bloom_bright"): shipped .cso
    // first, .hlsl compile as the dev fallback. No //@ state (post passes set
    // their own device state).
    bool LoadBuiltin(const char* stem, RtShader& out);

    // Load the project's environment cube map from <root>/assets/env
    // (env_px/nx/py/ny/pz/nz.png, deployed loose). Returns NULL when absent —
    // the runtime then binds its black default cube (no metal reflections).
    IDirect3DCubeTexture9* LoadEnvCube();

    // Combine the content root with a scene-relative path (slashes -> '\').
    std::string Resolve(const std::string& rel) const;

private:
    IDirect3DDevice9*             m_device;
    std::string                   m_root;
    std::map<std::string, RtMesh> m_meshes;
    std::map<std::string, RtShader> m_shaders;
    RtShader                      m_standard;
};
