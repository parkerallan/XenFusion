#pragma once

#include <xtl.h>
#include <map>
#include <vector>
#include <string>

#include "Content.h"    // RtMesh (shared render unit)
#include "StreamPak.h"

// Phase 3 of the streaming subsystem (STREAMING.md): asynchronous residency.
//
// A single worker thread pinned to HW thread 4 reads + LZX-decompresses entries
// OFF the render thread. The render thread drains completions each frame
// (budgeted) and does the D3D resource creation. A request for a not-yet-resident
// asset returns a placeholder (empty mesh = skip; 2x2 magenta texture) instead of
// stalling the frame, and swaps to the real asset once it lands.
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

    // Request a raw byte range through the shared worker's fixed 16 KiB,
    // sector-aligned page cache. Returns true and copies the resident bytes when
    // ready; false means queued/loading/failed. NULL out is a prefetch request.
    bool GetRawRange(const SpakEntry* entry, unsigned int offset, unsigned int bytes,
                     void* out);

private:
    enum State { StMissing = -1, StLoading = 0, StResident = 2 };

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
        unsigned int     bytes;
        unsigned int     lastUse;
        CacheTex() : state(StLoading), entry(NULL), bytes(0), lastUse(0) {}
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
    };
    struct Completion
    {
        unsigned int hash, offset, bytes;
        std::vector<BYTE> payload;
        bool ok, range;
    };

    IDirect3DTexture9* GetTextureByHash(unsigned int hash); // borrowed / placeholder
    void  Enqueue(unsigned int hash, const SpakEntry* entry);
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
    std::map<RangeKey, RangePage>     m_ranges;
    IDirect3DTexture9*                m_placeholder; // 2x2 magenta

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
