#pragma once

#include <xtl.h>
#include <map>
#include <vector>
#include <string>

#include "Content.h"    // RtMesh (shared render unit)
#include "StreamPak.h"
#include "text/CookedFont.h"

// Phase 3 of the streaming subsystem (STREAMING.md): asynchronous residency.
//
// A single worker thread pinned to HW thread 4 reads + LZX-decompresses entries
// OFF the render thread. The render thread drains completions each frame
// (budgeted) and does the D3D resource creation. A request for a not-yet-resident
// asset returns immediately (empty mesh = skip the draw) instead of stalling.
//
// Textures load PROGRESSIVELY: a large texture is cooked as a TXLO/TXHI pair, and
// the small-mip half — a few KB, and grouped at the front of the pak — makes it
// drawable (blurry) long before the full-resolution half arrives. The texture
// object is the same throughout, so the sharpen is a header write, not a swap.
// TXLO requests outrank TXHI on the worker so the blurry pass never queues behind
// full-resolution data.
//
// Threading contract: the cache maps + D3D resources are touched ONLY by the
// render thread. The worker touches only the (locked) request/completion queues
// and the pak's file handle. So the maps need no lock.
class StreamCache
{
public:
    StreamCache();
    ~StreamCache();

    // budgetMB caps resident asset memory; over budget, cold (not drawn in the
    // last few frames) resident assets are LRU-evicted. Assets in active use are
    // never evicted, so a working set larger than the budget exceeds it rather
    // than thrashing.
    void Init(IDirect3DDevice9* device, StreamPak* pak, unsigned int budgetMB = 128);
    void Shutdown();                                     // stops + joins the worker

    // Drain up to `budget` completed loads into live resources. Render thread,
    // once per frame, before drawing.
    void Update(unsigned int budget);

    // Resolve a mesh by scene-relative path. Returns the resident mesh, else NULL
    // when it is still loading OR absent; *inPak distinguishes them (true = in the
    // pak, so wait rather than fall back to a raw load).
    RtMesh* GetMesh(const std::string& relPath, bool* inPak);

    // Resolve a standalone texture by scene-relative path. Returns the texture as
    // soon as it is drawable — blurry at first for progressively-loaded ones — or
    // NULL while nothing is drawable yet and if absent (caller uses its default).
    IDirect3DTexture9* GetTexture(const std::string& relPath);

    // Resolve cooked font metadata and its referenced atlas. Returns NULL until
    // both asynchronous FONT and TX2D loads are resident. The returned objects
    // are borrowed and remain valid until cache eviction/shutdown.
    const text::CookedFont* GetFont(const std::string& relPath,
                                    IDirect3DTexture9** outAtlas);

    // Request a raw byte range through the shared worker's fixed 16 KiB,
    // sector-aligned page cache. Returns true and copies the resident bytes when
    // ready; false means queued/loading/failed. NULL out is a prefetch request.
    bool GetRawRange(const SpakEntry* entry, unsigned int offset, unsigned int bytes,
                     void* out);

    // --- Load-progress queries (scene loading) --------------------------------
    // The texture hashes a mesh's subsets reference. Only answerable once the
    // mesh itself is resident: the hashes live INSIDE the mesh payload, which is
    // why a scene's full asset set cannot be known before its meshes have landed.
    // Zero hashes (empty material slots) are skipped. Returns false if the mesh
    // is not resident yet.
    bool MeshTextureHashes(const std::string& relPath,
                           std::vector<unsigned int>& out) const;

    // True once a texture can actually be drawn — its small mips are registered
    // (blurry is fine) or it is fully resident. The full-resolution half keeps
    // streaming afterwards and is deliberately NOT waited on.
    bool TextureDrawable(unsigned int texHash) const;

private:
    // StLo: the texture's small mips are registered and it is drawable (blurry)
    // while its full-resolution half is still in flight. Only textures cooked as a
    // TXLO/TXHI pair pass through it.
    enum State { StMissing = -1, StLoading = 0, StLo = 1, StResident = 2 };

    struct CacheMesh
    {
        RtMesh                    mesh;
        std::vector<unsigned int> texHash; // 6 per subset (diffuse/normal/spec/emissive/metallic/clearcoat)
        int              state;
        const SpakEntry* entry;
        unsigned int     bytes;   // resident size, for the budget
        unsigned int     lastUse; // frame last requested (LRU)
        CacheMesh() : state(StLoading), entry(NULL), bytes(0), lastUse(0) {}
    };
    struct CacheTex
    {
        StreamTexture    tex;
        int              state;
        const SpakEntry* entry;
        const SpakEntry* hiEntry; // TXHI companion, NULL if cooked unsplit
        unsigned int     bytes;
        unsigned int     lastUse;
        CacheTex() : state(StLoading), entry(NULL), hiEntry(NULL), bytes(0), lastUse(0) {}
    };
    struct CacheFont
    {
        text::CookedFont font;
        int              state;
        const SpakEntry* entry;
        unsigned int     bytes;
        unsigned int     lastUse;
        CacheFont() : state(StLoading), entry(NULL), bytes(0), lastUse(0) {}
    };
    struct RangeKey
    {
        unsigned int hash, offset, bytes;
        bool operator<(const RangeKey& other) const
        {
            if (hash != other.hash) return hash < other.hash;
            if (offset != other.offset) return offset < other.offset;
            return bytes < other.bytes;
        }
    };
    struct RangePage
    {
        int state;
        unsigned int lastUse;
        std::vector<BYTE> payload;
        RangePage() : state(StLoading), lastUse(0) {}
    };
    struct Request
    {
        unsigned int hash, offset, bytes;
        const SpakEntry* entry;
        bool range;
        // A TXHI request is filed under its TEXTURE's hash (that is the key the
        // cache is indexed by) with this flag distinguishing it from the TXLO.
        bool hi;
    };
    struct Completion
    {
        unsigned int hash, offset, bytes;
        std::vector<BYTE> payload;
        bool ok, range, hi;
    };

    IDirect3DTexture9* GetTextureByHash(unsigned int hash); // borrowed, NULL until drawable
    void  Enqueue(unsigned int hash, const SpakEntry* entry, bool hi = false);
    void  EnqueueRange(const RangeKey& key, const SpakEntry* entry);
    void  BuildMeshFromPayload(const std::vector<BYTE>& payload, CacheMesh& cm);
    void  RefreshMeshTextures(CacheMesh& cm);
    void  EvictIfOverBudget();
    void  WorkerLoop();
    static DWORD WINAPI ThreadProc(LPVOID param);

    IDirect3DDevice9*                 m_device;
    StreamPak*                        m_pak;
    std::map<unsigned int, CacheMesh> m_meshes;    // key = nameHash(relPath)
    std::map<unsigned int, CacheTex>  m_textures;  // key = texture nameHash
    std::map<unsigned int, CacheFont> m_fonts;     // key = font path hash
    std::map<RangeKey, RangePage>     m_ranges;

    // Every TXLO payload, read once at Init. A texture's hashes live inside its
    // MESH payload, so a texture cannot even be REQUESTED until its mesh lands —
    // and the mesh is drawable the same frame it lands. Without these bytes already
    // in hand, every object is therefore visible for a full worker round trip
    // before any texture exists for it, showing the 1x1 default. They are a few KB
    // each and contiguous at the head of the pak, so this is one short read.
    std::map<unsigned int, std::vector<BYTE> > m_loBlobs;

    unsigned int m_frame;         // render-frame counter (LRU timestamps)
    size_t       m_budgetBytes;   // resident memory cap
    size_t       m_residentBytes; // current resident total
    size_t       m_rangeBudgetBytes;
    size_t       m_rangeResidentBytes;

    // Worker + cross-thread queues.
    HANDLE                  m_thread;
    HANDLE                  m_wake;     // auto-reset event: signal on new request
    volatile LONG           m_running;
    bool                    m_inited;
    CRITICAL_SECTION        m_reqLock;
    CRITICAL_SECTION        m_compLock;
    std::vector<Request>    m_requests;
    std::vector<Completion> m_completions;
};
