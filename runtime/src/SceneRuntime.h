#pragma once

#include "Content.h"
#include "SceneData.h"
#include "StreamCache.h"
#include "RuntimeAnimator.h"
#include "light/LightProbeGrid.h"
#include "video/VideoPlayer.h"
#include "audio/AudioPlayer.h"
#include "camera/CameraResolve.h"
#include "light/LightSelect.h"
#include "physics/PhysicsWorld.h"
#include "script/ScriptVM.h"
#include "script/ScriptTypes.h"
#include "input/InputState.h"
#include "gui/GuiContext.h"
#include "gui/GuiHost.h"

#include <xtl.h>

#include <map>
#include <string>
#include <vector>

// The stripped-down runtime renderer: the console analogue of the editor's
// SceneRenderer, minus everything editor-only. There is no grid, no gizmo, no
// selection highlight, and no keyboard/mouse camera — just the scene's meshes
// and custom-shader objects drawn from a fixed camera, straight to the back
// buffer. It reuses the editor's binary formats (.mesh / .scene) and material
// (standard.hlsl), so a scene authored in the editor renders the same here.
class SceneRuntime : public script::ScriptHost, public gui::HostAssets
{
public:
    SceneRuntime();

    // gui::HostAssets — the GUI core's only per-target seam. Textures and font
    // atlases come out of game.spak through the ASYNC StreamCache, so ids map
    // to PATHS and are resolved per frame: a menu asset can arrive a few frames
    // late (or be evicted), and caching the pointer would freeze the
    // placeholder in place forever.
    int  AcquireTexture(const char* relPath);
    bool TextureSize(int textureId, int& outWidth, int& outHeight);
    const text::FontMetrics* AcquireFont(const char* relPath);
    int  AcquireFontAtlas(const char* relPath);
    const gifanim::Info* AcquireGifFrame(const char* relPath, int frame,
                                         int& outTextureId, float& outGifSlice);

    // script::ScriptHost — input is stubbed until the input system lands; log
    // goes to OutputDebugString; find resolves a scene object name to its index.
    bool  InputButton(const char* name);
    float InputAxis(const char* name);
    bool  InputButtonPressed(const char* name);
    bool  InputButtonReleased(const char* name);
    void  Log(const char* msg);
    void  LogError(const char* msg);
    int   FindObject(const char* name);
    int   ObjectCount();
    const char* ObjectName(int index);
    void  TextSetValue(int objectIndex, const char* value);
    bool  GetObjectTransform(int objectIndex, float pos[3], float rot[3], float scale[3]);
    void  SetObjectTransform(int objectIndex, const float pos[3], const float rot[3],
                             const float scale[3], int mask);
    bool  GetObjectVisible(int objectIndex);
    void  SetObjectVisible(int objectIndex, bool visible);
    bool  SetActiveCamera(int objectIndex);
    int   GetActiveCamera();
    int   ObjectTagCount(int objectIndex);
    const char* ObjectTag(int objectIndex, int tagIndex);
    bool  AddObjectTag(int objectIndex, const char* tag);
    bool  RemoveObjectTag(int objectIndex, const char* tag);
    unsigned ObjectGeneration(int objectIndex);
    bool  ObjectAlive(int objectIndex);
    int   SpawnObject(int sourceObjectIndex, const char* name, const float pos[3]);
    bool  DestroyObject(int objectIndex);
    bool  LoadScene(const char* name, float minSeconds);
    bool  SceneIsLoading();
    float SceneProgress();
    const char* SceneName();
    bool  AttrGet(int objectIndex, int attrIndex, const char* field, attrfields::Value& out);
    bool  AttrSet(int objectIndex, int attrIndex, const char* field, const attrfields::Value& in);
    int   AttrCount(int objectIndex);
    const char* AttrType(int objectIndex, int attrIndex);
    void  VideoSetPlaying(int objectIndex, bool play, bool loop); // Lua video.play/stop
    bool  VideoIsPlaying(int objectIndex);
    void  AudioSetPlaying(int objectIndex, bool play); // Lua audio.*
    bool  AudioIsPlaying(int objectIndex);
    void  AudioSetVolume(int objectIndex, float volume);
    void  AudioSetPitch(int objectIndex, float pitch);
    void  AudioSetLoop(int objectIndex, bool loop);
    void  AnimatorSetFloat(int objectIndex, const char* name, float value);
    void  AnimatorSetBool(int objectIndex, const char* name, bool value);
    void  AnimatorSetTrigger(int objectIndex, const char* name);
    void  AnimatorSetState(int objectIndex, const char* name);

    // contentRoot is the deployed game root, e.g. "game:\". Loads the standard
    // material, the startup scene, and builds shared geometry. Returns false if
    // the device objects can't be created.
    bool Init(IDirect3DDevice9* device, const std::string& contentRoot);
    void Shutdown();

    // Load the bloom post shaders and hand them to the renderer, which runs the
    // glow chain between the tile resolve and Swap. Optional: missing .cso just
    // leaves the glow off.
    void InitBloom(class XboxRenderer& renderer);

    // Dynamic environment capture (reflections): render the scene into a small
    // cube map from the first reflective object's position, then build the
    // blurred chain roughness samples. MUST run OUTSIDE the tiling bracket —
    // call before XboxRenderer::BeginFrame(). Skips silently when the scene has
    // nothing metallic, rough or clearcoated.
    void PrepareFrame(float dt);
    void RenderDirectionalShadow();
    void RenderEnvCapture();

    // Draw the whole scene for one frame (device scene already begun by the
    // caller). dt advances gTime for animated custom shaders.
    void Render(float dt);

private:
    struct DrawItem
    {
        std::string model_path;
        D3DMATRIX   world;
        int         object_index; // for physics pose override
        bool        dynamic_lighting;
        bool        cast_shadow;
        bool        visible;      // obj:show()/obj:hide(); the item is kept either
                                  // way so the animator and stream stay warm
        float       scale[3];     // authored scale (collider is sized separately)
        RuntimeAnimator animator;
        std::vector<float> skin_palette;
        std::vector<int> bone_collider_handles;
        std::vector<RuntimeAnimator::BoneColliderPose> bone_collider_poses;
        DrawItem() : object_index(-1), dynamic_lighting(false), cast_shadow(false),
                     visible(true)
        { scale[0] = scale[1] = scale[2] = 1.0f; }
    };
    struct ShaderItem
    {
        std::string shader_path;
        std::string model_path;
        int   object_index;
        bool  visible;
        float pos[3];
        float rot[3];
        float scale[3];
        ShaderItem() : object_index(-1), visible(true)
        {
            pos[0] = pos[1] = pos[2] = 0.0f;
            rot[0] = rot[1] = rot[2] = 0.0f;
            scale[0] = scale[1] = scale[2] = 1.0f;
        }
    };

    // createItems = true builds the resource-owning DrawItem/ShaderItem shells
    // (Init only); false updates them in place from the live scene.
    void RebuildSceneLists(bool createItems);
    void BuildDrawLists() { RebuildSceneLists(true); }
    // Re-derive everything a script can change (transforms, visibility, lights,
    // the active camera, overlays) from m_scene. Driven by m_scene_dirty, so a
    // scene no script touches never pays for it.
    void RefreshSceneDerived() { RebuildSceneLists(false); }
    void LoadLightmaps(const std::string& startupScene);
    void BuildPhysics(); // create the Bullet world from Rigid Body / Trigger attributes
    void BuildScripts(); // load each object's .lua and run on_start
    // One object's collider descriptors — shared by the initial build and by
    // spawning, so a spawned body is set up exactly like an authored one.
    void AppendBodyDescs(int objectIndex, std::vector<phys::BodyDesc>& descs,
                         std::vector<std::vector<float>*>& geomPos,
                         std::vector<std::vector<unsigned int>*>& geomIdx);

    // --- Object lifetime + scene switching (see the Lua spawn/scene tables) ---
    void ResetObjectSlots();     // one live slot per scene object, generation 1
    void ApplyPendingObjects();  // end-of-frame spawn/destroy drain
    void ApplyPendingScene();    // end-of-frame scene swap
    std::vector<unsigned>      m_obj_gen;   // per-slot generation (handle stamp)
    std::vector<unsigned char> m_obj_alive; // 0 = free slot awaiting reuse
    std::vector<int>           m_pending_spawn;
    std::vector<int>           m_pending_destroy;
    std::string                m_pending_scene;

    // --- Phased scene load ---
    // A scene change runs as: parse the target and start streaming its meshes,
    // keep the CURRENT scene rendering (so a gui.* loading panel and the script
    // driving it stay alive), then hand over in one frame once everything is
    // resident. Waiting on meshes only is deliberate — textures are drawable
    // blurry from their small mips and sharpen in play, which is what the
    // progressive mip streaming exists for.
    // Progress is the fraction of the mesh BYTES this load has to read that have
    // arrived — the pak directory gives each mesh's size up front, so this is a
    // real measure of the streaming work and not a count of assets or a timer.
    // Hand-over additionally requires every texture those meshes use to be
    // DRAWABLE (small mips registered); the full-resolution halves keep streaming
    // into the new scene and are never waited on.
    enum LoadState { LoadIdle = 0, LoadPrefetch = 1 };
    static const float kSceneLoadTimeout;
    void  UpdateSceneLoad(float dt);
    unsigned int PakEntryBytes(unsigned int hash) const;

    int                       m_load_state;
    RtScene                   m_next_scene;  // parsed up front, swapped in at hand-over
    std::vector<std::string>  m_load_meshes;
    std::vector<unsigned int> m_load_mesh_bytes;
    unsigned int              m_load_mesh_total;
    float                     m_load_elapsed;
    float                     m_load_min_time;
    float                     m_load_progress;
    // Frames the load has ticked for. The hand-over needs at least two, so the
    // loading screen the script just built gets drawn before it is torn down.
    int                       m_load_frames;
    // Extract CPU collision geometry (positions, + indices for exact) for a mesh
    // collider straight from the pak's MSH2/MSH3 blob. Native-endian on the console;
    // skinned collision uses authored bind-pose geometry.
    bool LoadPakMeshGeometry(const std::string& relPath, std::vector<float>& pos,
                             std::vector<unsigned int>& idx, bool wantIndices);
    void DrawMesh(RtMesh* gm, const D3DMATRIX& world, const D3DMATRIX& vp, RtShader* mat,
                  bool blendPass, const std::vector<float>* skinPalette = NULL,
                  int objectIndex = -1);
    void DrawShaderItem(const ShaderItem& item, RtShader& shader, const D3DMATRIX& viewProj);
    IDirect3DTexture9* SolidTexture(D3DCOLOR argb);
    bool BuildGeometry();

    // Resolve a scene mesh: streamed from game.spak via the cache first, falling
    // back to the raw D3DX loader while both are shipped (pre phase-5 cutover).
    RtMesh* ResolveMesh(const std::string& relPath);

    IDirect3DDevice9*            m_device;
    std::string                  m_root;
    Content                      m_content;
    RtScene                      m_scene;

    struct LightmapMesh
    {
        IDirect3DVertexBuffer9* vb;
        IDirect3DIndexBuffer9* ib;
        unsigned int vertexCount;
        LightmapMesh() : vb(NULL), ib(NULL), vertexCount(0) {}
        void Release()
        {
            if (ib) { ib->Release(); ib = NULL; }
            if (vb) { vb->Release(); vb = NULL; }
            vertexCount = 0;
        }
    };
    IDirect3DVertexDeclaration9* m_lightmap_mesh_decl;
    std::map<int, LightmapMesh>  m_lightmap_meshes;
    std::string                  m_lightmap0_path;
    std::string                  m_lightmap1_path;

    // Bloom post shaders (owned here, used by XboxRenderer's glow chain).
    RtShader                     m_bloom_bright;
    RtShader                     m_bloom_blur;
    RtShader                     m_bloom_combine;

    // Spot volumetric beam ("Project Light Shaft"): beam.hlsl over a unit-cone
    // VB (apex origin, +Z, radius = z), scaled to tan(outer) * range by each
    // beam's world matrix. Captured in BuildDrawLists, drawn additively after
    // the translucents. Mirrors the editor's SceneRenderer.
    RtShader                     m_beam;
    IDirect3DVertexBuffer9*      m_beam_vb; // 24-segment fan, 72 verts, 44B each
    struct SpotBeam
    {
        D3DMATRIX world;
        float     color[4]; // rgb premultiplied: spot color * intensity * beam * gain
        float     apex[4];  // world apex (PS c2) — the PS rebuilds the cone
        float     axis[4];  // normal analytically: xyz = unit dir, w = tan(outer)
    };
    SpotBeam                     m_spot_beams[2];
    int                          m_spot_beam_count;

    IDirect3DVertexDeclaration9* m_mesh_decl;
    IDirect3DVertexDeclaration9* m_skin_mesh_decl;
    IDirect3DTexture9*           m_def_white;
    IDirect3DTexture9*           m_def_normal;
    IDirect3DTexture9*           m_def_black;
    IDirect3DCubeTexture9*       m_def_envcube; // black cube: env fallback
    IDirect3DCubeTexture9*       m_env;         // static env map (assets/env), fallback

    // Directional shadow map: dynamic casters only into a 1024^2 orthographic
    // map, rendered before BeginTiling into an EDRAM RT at Base 0 and resolved
    // to a texture sampled by standard.hlsl (s10/c23/c225-c228).
    IDirect3DSurface9*           m_shadowRT;
    IDirect3DSurface9*           m_shadowDepth;
    IDirect3DTexture9*           m_shadowTexture;
    D3DMATRIX                    m_shadowMatrix;
    bool                         m_shadow_enabled;
    bool                         m_shadow_valid;

    // Dynamic environment capture: EDRAM working target + depth (Base 0 —
    // aliases the tile targets, legal because the capture runs before
    // BeginTiling) resolved face-by-face into the cube texture.
    IDirect3DSurface9*           m_envRT;
    IDirect3DSurface9*           m_envDepth;
    IDirect3DCubeTexture9*       m_envDynCube;
    // Blurred copy of the capture, envcube::kMips levels deep — what actually
    // binds to s5. Roughness picks a level (standard.hlsl texCUBElod).
    IDirect3DCubeTexture9*       m_envBlurCube;
    // Scratch chain the blur ping-pongs against: the filter is progressive
    // (level L reads level L-1), so the source and destination cubes swap.
    IDirect3DCubeTexture9*       m_envBlurTmp;
    RtShader                     m_env_blur;     // env_blur.hlsl (optional)
    bool                         m_env_captured; // this frame -> bind m_envBlurCube
    // One capture face: frame constants + both material passes over the draw
    // items (via DrawMesh), skipping the captured object itself.
    void DrawModelsForEnv(const D3DMATRIX& vp, const float* eye, int skipItem);
    // Cone-filter the capture into m_envBlurCube's levels (env_blur.hlsl).
    void BuildEnvBlurChain();
    bool BuildDirectionalShadowMatrix(D3DMATRIX& outMatrix);

    // --- Skybox ("Skybox" attribute) ---
    // An equirectangular image drawn as the scene background: a screen quad at
    // the far plane, so geometry occludes it through the depth test. Also drawn
    // into each face of the capture above, so reflections show the sky rather
    // than the flat colour the face was cleared to. First Skybox in scene order
    // wins; captured in BuildDrawLists, consumed in Render.
    std::string                  m_sky_path;     // pak-relative, empty = no sky
    float                        m_sky_rotation; // degrees, yaw
    RtShader                     m_skybox;       // skybox.hlsl (optional)
    void RenderSkybox(const D3DMATRIX& view, const D3DMATRIX& proj, bool depthTest);

    // Unit quad + unit cube (mesh vertex layout) for standalone shaders.
    IDirect3DVertexBuffer9*      m_quad_vb;
    IDirect3DIndexBuffer9*       m_quad_ib;
    IDirect3DVertexBuffer9*      m_cube_vb;
    IDirect3DIndexBuffer9*       m_cube_ib;

    // Fixed orbit camera (the editor's default framing; no input on the console).
    // Used only when the scene has no active "Camera" attribute.
    float m_yaw;
    float m_pitch;
    float m_distance;
    float m_target[3];
    float m_eye[3];

    // Active scene camera ("Camera" attribute with active=true, first wins).
    // BuildDrawLists records WHICH camera (object/attribute index + target
    // object index); Render resolves the view per frame through the shared
    // camera/CameraResolve.h so Follow/Track see live physics/script motion.
    bool  m_has_cam;
    int   m_cam_object;      // index into m_scene.objects
    int   m_cam_attr;        // index into that object's attributes
    int   m_cam_target;      // follow target object index (-1 = none)
    float m_cam_fov;  // vertical FOV, degrees
    float m_cam_near;
    float m_cam_far;

    // Per-frame camera state: track advancement + follow smoothing.
    float m_track_dist;
    float m_track_speed;
    camr::FollowSmooth m_follow_smooth;

    // Scene lights captured in BuildDrawLists — ready-made PS constant payloads
    // (c0 / c2 / c6 / c7-c10 / c11-c14 / c16-c21), mirroring the editor's
    // SceneRenderer. The legacy fixed sun/ambient are used when the scene has
    // no light attributes at all.
    float m_light_dir[4];
    float m_light_col[4];
    std::vector<lsel::PointLight> m_point_lights;
    std::vector<lsel::SpotLight>  m_spot_lights;
    float m_point_pos[4][4]; // xyz + w = 1/range^2
    float m_point_col[4][4]; // rgb * intensity; zero = unused slot
    float m_spot_pos[2][4];  // xyz + w = 1/range
    float m_spot_dir[2][4];  // xyz = beam dir (+Z fwd), w = cos(inner half-angle)
    float m_spot_col[2][4];  // rgb * intensity (zero = unused), w = cos(outer)
    float m_ambient[4];      // environment-light sum or the legacy default
    float m_baked_ambient[4]; // baked env is global and never directionally modulated
    lprobe::Grid m_probe_grid;

    float m_time;
    float m_frame_dt;
    bool  m_frame_prepared;
    // Set by the ScriptHost setters when a script mutates m_scene; consumed by
    // PrepareFrame, which then re-derives the draw state.
    bool  m_scene_dirty;

    std::vector<DrawItem>   m_draw_items;
    std::vector<ShaderItem> m_shader_items;

    struct ImageItem
    {
        std::string path;
        float x, y, w, h;
        bool  stretch;
        bool  lock_aspect;
        float tint[3];
        float alpha;
        int   priority;
        int   sequence;
        // .gif only. `key` is "<object>#<attrIndex>" so playback belongs to the
        // attribute, not the file: two objects showing the same GIF run their
        // own clocks. Same idiom as VideoItem.
        int         play_mode;
        std::string key;
        ImageItem() : x(0), y(0), w(0), h(0), stretch(false), lock_aspect(false),
                  alpha(1.0f), priority(1), sequence(0), play_mode(2)
        { tint[0] = tint[1] = tint[2] = 1.0f; }
    };

    // A flat block. Image's item minus the texture, so the draw needs no cache
    // lookup and no aspect fix-up — w/h are used exactly as authored.
    struct ColorItem
    {
        float x, y, w, h;
        bool  stretch;
        float rgb[3];
        float alpha;
        int   priority;
        int   sequence;
        ColorItem() : x(0), y(0), w(0), h(0), stretch(false),
                      alpha(1.0f), priority(1), sequence(0)
        { rgb[0] = rgb[1] = rgb[2] = 1.0f; }
    };

    struct TextItem
    {
        std::string object;
        std::string fontPath;
        std::string value;
        float x, y, w, h;
        float fontSize;
        float color[3];
        float alpha;
        bool  lockAspect;
        int   priority;
        int   sequence;
        TextItem() : x(0), y(0), w(0), h(0), fontSize(32.0f), alpha(1.0f),
                     lockAspect(false), priority(1), sequence(0)
        { color[0] = color[1] = color[2] = 1.0f; }
    };

    // --- Image/video overlays ---
    // Screen-space quads over the finished 3D scene, drawn INSIDE the tiled
    // pass (they replay per band) through the video.hlsl builtin. The pak's
    // raw VIDE entry is range-streamed by the shared VideoPlayer; separate
    // alpha blending zeroes the glow mask under the video so bloom never
    // halos through it.
    struct VideoItem
    {
        std::string  key;      // stream identity: object name + '#' + attr index
        std::string  object;   // owning object's name (Lua video.* lookups)
        std::string  path;     // scene-relative .mpg
        float x, y, w, h;      // 1280x720 reference space
        bool  stretch;
        bool  lock_aspect;
        float tint[3];
        float alpha;
        int   priority;        // higher = drawn behind
        int   sequence;        // scene order for equal priorities
        int   play_mode;
        float volume;
        bool  muted;
        VideoItem() : x(0), y(0), w(0), h(0), stretch(false), lock_aspect(true),
                      alpha(1.0f), priority(1), sequence(0), play_mode(2), volume(1.0f), muted(false)
        { tint[0] = tint[1] = tint[2] = 1.0f; }
    };

    // The three L8 plane textures for one stream, double-buffered so an upload
    // never touches the set the GPU is still reading (LIN_ formats: linear
    // layout, plain row memcpy, no tiling or endian work — L8 is bytes).
    struct RtVideoTex
    {
        std::string        key;
        IDirect3DTexture9* y[2];
        IDirect3DTexture9* cb[2];
        IDirect3DTexture9* cr[2];
        int      yW, yH, cW, cH;
        unsigned lastFrame; // vid::Frame::frameId last uploaded
        int      cur;       // set the last upload went to (bind this one)
        bool     used;      // reconcile mark
        RtVideoTex() : yW(0), yH(0), cW(0), cH(0), lastFrame(0), cur(0), used(false)
        { y[0] = y[1] = cb[0] = cb[1] = cr[0] = cr[1] = NULL; }
    };

    void        RenderOverlay(float dt);
    RtVideoTex* EnsureVideoTex(const std::string& key, const vid::Frame& frame);

    // --- Lua-scriptable GUI (the `gui` table; ../src/gui is shared) ---
    // Drawn straight after the overlay, INSIDE the tiling bracket: a clip-space
    // quad replays fine per band, and the same separate-alpha blend the overlay
    // uses zeroes the glow mask underneath so bloom can never halo through a
    // menu.
    void RenderGui();
    // Texture ids are indices into this table. Fonts, plain textures and
    // animated GIFs share the id space; `kind` picks which cache lookup
    // resolves it. Ids stay valid across streaming because the resolve happens
    // per frame, not at Acquire time.
    struct GuiAsset
    {
        enum Kind { KTexture = 0, KFont, KGif };
        std::string path;
        int         kind;
        GuiAsset() : kind(KTexture) {}
    };
    IDirect3DTexture9* GuiTexture(int textureId);
    std::vector<GuiAsset>      m_gui_assets;
    std::map<std::string, int> m_gui_texture_ids;
    std::map<std::string, int> m_gui_font_atlas_ids;
    // GIFs get their own id table for the same reason fonts do: one path must
    // never resolve to an asset of the wrong kind.
    std::map<std::string, int> m_gui_gif_ids;
    gui::Context               m_gui;
    RtShader                   m_gui_shader;
    RtShader                   m_gui_array_shader; // gui.image on an animated .gif
    // The item's stream key / play mode with any script override applied
    // (evaluated at draw time, after this frame's scripts ran).
    std::string VideoKeyFor(const VideoItem& item) const;
    int         VideoModeFor(const VideoItem& item) const;

    // --- Audio ("Audio" attribute; the per-frame reconciler) ---
    // Captured once in BuildDrawLists (scene data is static), reconciled every
    // frame by UpdateAudio: playing attributes become AudioPlayer streams fed
    // from the pak's raw AUDI byte windows (loose-file dev fallback). 3D
    // fields are carried now, applied in the spatial phase.
    struct AudioItem
    {
        std::string key;      // object name + '#' + attr index
        std::string object;   // owning object's name (Lua lookups)
        std::string path;     // scene-relative .mp2
        int   object_index;   // live pose lookup (spatial phase)
        bool  play;
        bool  loop;
        float volume;
        float pitch;
        int   audio_class;
        int   priority;
        int   load_mode;
        bool  spatial;
        float min_dist, max_dist, doppler;
        AudioItem() : object_index(-1), play(false), loop(false),
                      volume(1.0f), pitch(1.0f), audio_class(aud::AudioEffect),
                      priority(0), load_mode(aud::AudioLoadAuto), spatial(true),
                      min_dist(1.0f), max_dist(50.0f), doppler(1.0f) {}
    };
    void UpdateAudio(float dt, const D3DMATRIX& view);
    // Script overrides (object name -> partial state); -1 = "no override".
    // The generation restarts a stopped clip from the top (new stream key).
    struct AudioOverride
    {
        int      play;
        int      loop;
        float    volume;
        float    pitch;
        unsigned gen;
        AudioOverride() : play(-1), loop(-1), volume(-1.0f), pitch(-1.0f), gen(0) {}
    };
    std::string AudioKeyFor(const AudioItem& item) const;
    std::map<std::string, AudioOverride> m_audio_overrides;
    std::vector<AudioItem> m_audio_items;
    // Opened once and also used as AudioPlayer's serialized range reader.
    StreamPak              m_pak;
    aud::AudioPlayer       m_audio;

    std::vector<ImageItem>  m_image_items;
    std::vector<ColorItem>  m_color_items;
    std::vector<TextItem>   m_text_items;
    std::map<std::string, std::string> m_text_overrides;
    std::vector<VideoItem>  m_video_items;
    std::vector<RtVideoTex> m_video_tex;
    // Script play/stop overrides (object name -> mode + restart generation);
    // the parsed scene's play_mode stays authored. The generation is folded
    // into the stream key so video.play on a stopped/finished video decodes
    // a fresh stream from the top.
    struct VideoOverride
    {
        int      mode; // vid::PlayMode
        unsigned gen;
        VideoOverride() : mode(0), gen(0) {}
    };
    std::map<std::string, VideoOverride> m_video_overrides;
    vid::VideoPlayer        m_video;
    RtShader                m_image_shader;
    RtShader                m_image_array_shader; // .gif overlays (tex3D over the frame stack)
    RtShader                m_color_shader;
    RtShader                m_text_shader;
    RtShader                m_video_shader;

    // GIF playback clocks, keyed by ImageItem::key. Deliberately NOT cleared
    // with m_image_items: BuildDrawLists re-runs whenever a script touches an
    // attribute, and rewinding every GIF on that would make an animation
    // stutter back to frame 1. Only a scene unload resets these.
    std::map<std::string, gifanim::Playback> m_gif_play;

    // Streaming: the pak is driven through the residency cache.
    StreamCache m_cache;

    // Physics (Bullet) — same wrapper the editor preview uses. Built in Init from
    // the scene's Rigid Body / Trigger attributes, stepped each Render(dt).
    phys::PhysicsWorld      m_phys;
    std::vector<phys::Pose> m_phys_poses;

    // Lua scripting — same VM the editor uses. Built in Init, stepped before m_phys.
    script::ScriptVM        m_script;

    // Controller snapshot, polled from XInput each frame; read by the ScriptHost.
    input::InputState       m_input;
};
