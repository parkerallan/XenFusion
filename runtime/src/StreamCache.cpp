#include "StreamCache.h"
#include "Endian.h"

#include <stdio.h>

namespace
{
    IDirect3DTexture9* MakeMagenta2x2(IDirect3DDevice9* dev)
    {
        IDirect3DTexture9* t = NULL;
        if (FAILED(dev->CreateTexture(2, 2, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t, NULL)))
            return NULL;
        D3DLOCKED_RECT r;
        if (SUCCEEDED(t->LockRect(0, &r, NULL, 0)))
        {
            D3DCOLOR* px = (D3DCOLOR*)r.pBits;
            for (int i = 0; i < 4; ++i) px[i] = D3DCOLOR_ARGB(255, 255, 0, 255);
            t->UnlockRect(0);
        }
        return t;
    }
}

StreamCache::StreamCache()
    : m_device(NULL), m_pak(NULL), m_placeholder(NULL),
      m_thread(NULL), m_wake(NULL), m_running(0), m_inited(false),
      m_frame(0), m_budgetBytes(0), m_residentBytes(0)
{
}

namespace { const unsigned int kEvictGraceFrames = 2; } // don't evict just-used assets

StreamCache::~StreamCache()
{
    Shutdown();
}

// ---------------------------------------------------------------------------
// Worker thread
// ---------------------------------------------------------------------------
DWORD WINAPI StreamCache::ThreadProc(LPVOID param)
{
    ((StreamCache*)param)->WorkerLoop();
    return 0;
}

void StreamCache::WorkerLoop()
{
    LARGE_INTEGER freq; QueryPerformanceFrequency(&freq);

    while (m_running)
    {
        WaitForSingleObject(m_wake, INFINITE);
        for (;;)
        {
            if (!m_running)
                break;

            // Pop the lowest-diskOffset pending request (march the DVD head forward).
            Request r; bool have = false;
            EnterCriticalSection(&m_reqLock);
            if (!m_requests.empty())
            {
                size_t best = 0;
                for (size_t i = 1; i < m_requests.size(); ++i)
                    if (m_requests[i].entry->diskOffset < m_requests[best].entry->diskOffset)
                        best = i;
                r = m_requests[best];
                m_requests[best] = m_requests.back();
                m_requests.pop_back();
                have = true;
            }
            LeaveCriticalSection(&m_reqLock);
            if (!have)
                break;

            // The heavy part: read + decompress, off the render thread.
            LARGE_INTEGER t0, t1;
            QueryPerformanceCounter(&t0);
            std::vector<BYTE> blob;
            bool ok = m_pak->ReadBlob(r.entry, blob);
            QueryPerformanceCounter(&t1);

            const double ms = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)freq.QuadPart;
            const double mb = (double)r.entry->uncompressedSize / (1024.0 * 1024.0);
            char msg[160];
            sprintf_s(msg, sizeof(msg),
                      "cache: worker loaded 0x%08x  %u->%u bytes in %.2f ms (%.1f MB/s decoded)%s\n",
                      r.hash, r.entry->compressedSize, r.entry->uncompressedSize, ms,
                      ms > 0.0 ? mb / (ms / 1000.0) : 0.0, ok ? "" : "  [FAIL]");
            OutputDebugStringA(msg);

            EnterCriticalSection(&m_compLock);
            m_completions.resize(m_completions.size() + 1);
            m_completions.back().hash = r.hash;
            m_completions.back().ok   = ok;
            m_completions.back().payload.swap(blob);
            LeaveCriticalSection(&m_compLock);
        }
    }
}

// ---------------------------------------------------------------------------
// Lifetime
// ---------------------------------------------------------------------------
void StreamCache::Init(IDirect3DDevice9* device, StreamPak* pak, unsigned int budgetMB)
{
    m_device = device;
    m_pak    = pak;
    m_budgetBytes   = (size_t)budgetMB * 1024u * 1024u;
    m_residentBytes = 0;
    m_frame = 0;
    m_placeholder = MakeMagenta2x2(device);

    InitializeCriticalSection(&m_reqLock);
    InitializeCriticalSection(&m_compLock);
    m_wake = CreateEvent(NULL, FALSE, FALSE, NULL); // auto-reset, initially unsignaled
    m_inited = true;

    m_running = 1;
    m_thread = CreateThread(NULL, 0, ThreadProc, this, 0, NULL);
    if (m_thread)
        XSetThreadProcessor(m_thread, 4); // pin to HW thread 4 (of 6)
}

void StreamCache::Shutdown()
{
    if (m_thread)
    {
        InterlockedExchange(&m_running, 0);
        if (m_wake) SetEvent(m_wake);
        WaitForSingleObject(m_thread, INFINITE);
        CloseHandle(m_thread);
        m_thread = NULL;
    }
    if (m_wake) { CloseHandle(m_wake); m_wake = NULL; }
    if (m_inited)
    {
        DeleteCriticalSection(&m_reqLock);
        DeleteCriticalSection(&m_compLock);
        m_inited = false;
    }

    // Meshes own only their VB/IB (COM); texture pointers are borrowed from
    // m_textures, so release the buffers here but NOT the textures.
    for (std::map<unsigned int, CacheMesh>::iterator it = m_meshes.begin(); it != m_meshes.end(); ++it)
    {
        if (it->second.mesh.ib) { it->second.mesh.ib->Release(); it->second.mesh.ib = NULL; }
        if (it->second.mesh.vb) { it->second.mesh.vb->Release(); it->second.mesh.vb = NULL; }
    }
    m_meshes.clear();
    for (std::map<unsigned int, CacheTex>::iterator it = m_textures.begin(); it != m_textures.end(); ++it)
        it->second.tex.Release();
    m_textures.clear();

    if (m_placeholder) { m_placeholder->Release(); m_placeholder = NULL; }
    m_requests.clear();
    m_completions.clear();
    m_residentBytes = 0;
    m_device = NULL;
    m_pak    = NULL;
}

// ---------------------------------------------------------------------------
// Requests + render-thread completion drain
// ---------------------------------------------------------------------------
void StreamCache::Enqueue(unsigned int hash, const SpakEntry* entry)
{
    EnterCriticalSection(&m_reqLock);
    Request r; r.hash = hash; r.entry = entry;
    m_requests.push_back(r);
    LeaveCriticalSection(&m_reqLock);
    if (m_wake) SetEvent(m_wake);
}

void StreamCache::Update(unsigned int budget)
{
    ++m_frame;

    unsigned int processed = 0;
    for (unsigned int b = 0; b < budget; ++b)
    {
        Completion c; bool have = false;
        EnterCriticalSection(&m_compLock);
        if (!m_completions.empty())
        {
            c.hash = m_completions.back().hash;
            c.ok   = m_completions.back().ok;
            c.payload.swap(m_completions.back().payload);
            m_completions.pop_back();
            have = true;
        }
        LeaveCriticalSection(&m_compLock);
        if (!have)
            break;
        ++processed;

        // Accounted size = the entry's sysmem + vidmem (texels / VB+IB). Set lastUse
        // to the current frame so a just-loaded asset isn't immediately evicted.
        std::map<unsigned int, CacheMesh>::iterator mit = m_meshes.find(c.hash);
        if (mit != m_meshes.end())
        {
            if (c.ok) BuildMeshFromPayload(c.payload, mit->second);
            if (c.ok && mit->second.mesh.vb)
            {
                mit->second.state   = StResident;
                mit->second.bytes   = mit->second.entry ? (mit->second.entry->sysMemSize + mit->second.entry->vidMemSize) : 0;
                mit->second.lastUse = m_frame;
                m_residentBytes    += mit->second.bytes;
            }
            else mit->second.state = StMissing;
            continue;
        }
        std::map<unsigned int, CacheTex>::iterator tit = m_textures.find(c.hash);
        if (tit != m_textures.end())
        {
            if (c.ok)
                m_pak->RegisterTextureFromBlob(c.payload.empty() ? NULL : &c.payload[0],
                                               (unsigned int)c.payload.size(), tit->second.tex);
            if (c.ok && tit->second.tex.tex)
            {
                tit->second.state   = StResident;
                tit->second.bytes   = tit->second.entry ? (tit->second.entry->sysMemSize + tit->second.entry->vidMemSize) : 0;
                tit->second.lastUse = m_frame;
                m_residentBytes    += tit->second.bytes;
            }
            else tit->second.state = StMissing;
        }
    }

    if (processed)
    {
        char msg[128];
        sprintf_s(msg, sizeof(msg), "cache: +%u resident this frame, %u MB / %u MB budget\n",
                  processed, (unsigned int)(m_residentBytes / (1024 * 1024)),
                  (unsigned int)(m_budgetBytes / (1024 * 1024)));
        OutputDebugStringA(msg);
    }

    EvictIfOverBudget();
}

// LRU-evict resident assets that haven't been drawn in the last few frames until
// back under budget. Assets in active use are never evicted, so a working set
// larger than the budget simply exceeds it (no thrash) — bounded memory kicks in
// when parts of the scene go cold (culling / camera movement).
void StreamCache::EvictIfOverBudget()
{
    if (m_budgetBytes == 0)
        return;

    while (m_residentBytes > m_budgetBytes)
    {
        // Find the coldest evictable resident asset across both maps.
        int   kind = -1;            // 0 = mesh, 1 = texture
        unsigned int bestHash = 0;
        unsigned int bestLast = 0xFFFFFFFFu;

        for (std::map<unsigned int, CacheMesh>::iterator it = m_meshes.begin(); it != m_meshes.end(); ++it)
            if (it->second.state == StResident && it->second.lastUse + kEvictGraceFrames <= m_frame &&
                it->second.lastUse < bestLast)
            { bestLast = it->second.lastUse; bestHash = it->first; kind = 0; }

        for (std::map<unsigned int, CacheTex>::iterator it = m_textures.begin(); it != m_textures.end(); ++it)
            if (it->second.state == StResident && it->second.lastUse + kEvictGraceFrames <= m_frame &&
                it->second.lastUse < bestLast)
            { bestLast = it->second.lastUse; bestHash = it->first; kind = 1; }

        if (kind < 0)
            break; // everything resident is in active use — exceed budget gracefully

        unsigned int freed = 0;
        if (kind == 0)
        {
            std::map<unsigned int, CacheMesh>::iterator it = m_meshes.find(bestHash);
            if (it->second.mesh.ib) { it->second.mesh.ib->Release(); it->second.mesh.ib = NULL; }
            if (it->second.mesh.vb) { it->second.mesh.vb->Release(); it->second.mesh.vb = NULL; }
            freed = it->second.bytes;
            m_meshes.erase(it); // next request re-enqueues it
        }
        else
        {
            std::map<unsigned int, CacheTex>::iterator it = m_textures.find(bestHash);
            it->second.tex.Release();
            freed = it->second.bytes;
            m_textures.erase(it);
        }
        m_residentBytes -= freed;

        char msg[128];
        sprintf_s(msg, sizeof(msg), "cache: evicted 0x%08x (%s), freed %u KB, resident %u MB / %u MB\n",
                  bestHash, kind == 0 ? "mesh" : "tex", freed / 1024,
                  (unsigned int)(m_residentBytes / (1024 * 1024)), (unsigned int)(m_budgetBytes / (1024 * 1024)));
        OutputDebugStringA(msg);
    }
}

void StreamCache::BuildMeshFromPayload(const std::vector<BYTE>& blob, CacheMesh& cm)
{
    if (blob.size() < spak::kMeshHeaderBytes)
        return;
    const unsigned char* p = &blob[0];
    if (endian::LoadU32BE(p + 0) != spak::kMeshMagic)
        return;
    const unsigned int vcount      = endian::LoadU32BE(p + 4);
    const unsigned int icount      = endian::LoadU32BE(p + 8);
    const unsigned int subsetCount = endian::LoadU32BE(p + 12);
    if (vcount == 0 || icount == 0 || subsetCount == 0 || subsetCount > 1024)
        return;

    const size_t subBytes = (size_t)subsetCount * spak::kMeshSubsetBytes;
    const size_t vbytes   = (size_t)vcount * spak::kMeshVertexBytes;
    const size_t ibytes   = (size_t)icount * 4;
    if (spak::kMeshHeaderBytes + subBytes + vbytes + ibytes > blob.size())
        return;

    // Per-material subsets: index range + alpha mode + texture hashes.
    cm.texHash.assign((size_t)subsetCount * 6, 0);
    for (unsigned int i = 0; i < subsetCount; ++i)
    {
        const unsigned char* sp = p + spak::kMeshHeaderBytes + (size_t)i * spak::kMeshSubsetBytes;
        RtSubset sub;
        sub.indexStart = endian::LoadU32BE(sp + 0);
        sub.indexCount = endian::LoadU32BE(sp + 4);
        const unsigned int alpha = endian::LoadU32BE(sp + 8);
        const unsigned int kind  = alpha & spak::kAlphaKindMask;
        sub.alpha = (kind == spak::kAlphaCutout) ? RtCutout
                  : (kind == spak::kAlphaBlend)  ? RtBlend : RtOpaque;
        sub.normalHasHeight = (alpha & spak::kAlphaHeightBit) != 0;
        cm.texHash[(size_t)i * 6 + 0] = endian::LoadU32BE(sp + 12);
        cm.texHash[(size_t)i * 6 + 1] = endian::LoadU32BE(sp + 16);
        cm.texHash[(size_t)i * 6 + 2] = endian::LoadU32BE(sp + 20);
        cm.texHash[(size_t)i * 6 + 3] = endian::LoadU32BE(sp + 24);
        cm.texHash[(size_t)i * 6 + 4] = endian::LoadU32BE(sp + 28);
        cm.texHash[(size_t)i * 6 + 5] = endian::LoadU32BE(sp + 32);
        if ((size_t)sub.indexStart + sub.indexCount > icount)
        { cm.mesh.subsets.clear(); return; }
        cm.mesh.subsets.push_back(sub);
    }

    const size_t bufOff = spak::kMeshHeaderBytes + subBytes;

    // Payload VB/IB are already native-endian (baked big-endian) — copy straight in.
    if (FAILED(m_device->CreateVertexBuffer((UINT)vbytes, 0, 0, D3DPOOL_MANAGED, &cm.mesh.vb, NULL)))
    {
        cm.mesh.subsets.clear();
        return;
    }
    {
        void* dst = NULL;
        cm.mesh.vb->Lock(0, 0, &dst, 0);
        memcpy(dst, p + bufOff, vbytes);
        cm.mesh.vb->Unlock();
    }
    if (FAILED(m_device->CreateIndexBuffer((UINT)ibytes, 0, D3DFMT_INDEX32, D3DPOOL_MANAGED, &cm.mesh.ib, NULL)))
    {
        cm.mesh.vb->Release(); cm.mesh.vb = NULL;
        cm.mesh.subsets.clear();
        return;
    }
    {
        void* dst = NULL;
        cm.mesh.ib->Lock(0, 0, &dst, 0);
        memcpy(dst, p + bufOff + vbytes, ibytes);
        cm.mesh.ib->Unlock();
    }

    cm.mesh.vertexCount = vcount;
    cm.mesh.indexCount  = icount;
    // Texture pointers resolved lazily by RefreshMeshTextures (they may still be loading).
}

// ---------------------------------------------------------------------------
// Lookup (render thread)
// ---------------------------------------------------------------------------
IDirect3DTexture9* StreamCache::GetTextureByHash(unsigned int hash)
{
    if (hash == 0)
        return NULL; // slot has no texture -> mesh uses its default

    std::map<unsigned int, CacheTex>::iterator it = m_textures.find(hash);
    if (it == m_textures.end())
    {
        CacheTex ct;
        const SpakEntry* e = m_pak ? m_pak->Find(hash) : NULL;
        ct.entry = e;
        if (e && e->type == spak::kTypeTex2D) { ct.state = StLoading; m_textures[hash] = ct; Enqueue(hash, e); }
        else                                  { ct.state = StMissing; m_textures[hash] = ct; }
        it = m_textures.find(hash);
    }
    CacheTex& ct = it->second;
    if (ct.state == StResident) { ct.lastUse = m_frame; return ct.tex.tex; } // touch for LRU
    if (ct.state == StMissing)  return NULL;         // absent -> default at draw
    return m_placeholder;                            // loading -> magenta
}

void StreamCache::RefreshMeshTextures(CacheMesh& cm)
{
    for (size_t i = 0; i < cm.mesh.subsets.size() && i * 6 + 5 < cm.texHash.size(); ++i)
    {
        RtSubset& s = cm.mesh.subsets[i];
        s.diffuse   = GetTextureByHash(cm.texHash[i * 6 + 0]);
        s.normal    = GetTextureByHash(cm.texHash[i * 6 + 1]);
        s.specular  = GetTextureByHash(cm.texHash[i * 6 + 2]);
        s.emissive  = GetTextureByHash(cm.texHash[i * 6 + 3]);
        s.metallic  = GetTextureByHash(cm.texHash[i * 6 + 4]);
        s.clearcoat = GetTextureByHash(cm.texHash[i * 6 + 5]);
    }
}

RtMesh* StreamCache::GetMesh(const std::string& relPath, bool* inPak)
{
    if (inPak) *inPak = false;
    if (relPath.empty() || !m_device)
        return NULL;

    const unsigned int hash = spak::NameHash(relPath.c_str());
    std::map<unsigned int, CacheMesh>::iterator it = m_meshes.find(hash);
    if (it == m_meshes.end())
    {
        CacheMesh cm;
        const SpakEntry* e = m_pak ? m_pak->Find(hash) : NULL;
        cm.entry = e;
        if (e && e->type == spak::kTypeMesh) { cm.state = StLoading; m_meshes[hash] = cm; Enqueue(hash, e); }
        else                                 { cm.state = StMissing; m_meshes[hash] = cm; }
        it = m_meshes.find(hash);
    }

    CacheMesh& cm = it->second;
    if (cm.state == StMissing)
        return NULL;                  // not in pak — caller falls back to raw load
    if (inPak) *inPak = true;
    if (cm.state == StResident)
    {
        cm.lastUse = m_frame;         // touch for LRU (also touches its textures)
        RefreshMeshTextures(cm);
        return &cm.mesh;
    }
    return NULL;                       // in pak but still loading — skip this frame
}
