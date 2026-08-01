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
    m_frame(0), m_budgetBytes(0), m_residentBytes(0),
    m_rangeBudgetBytes(8u * 1024u * 1024u), m_rangeResidentBytes(0)
{
}

namespace
{
    const unsigned int kEvictGraceFrames = 2; // don't evict just-used assets
}

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
                    if (m_requests[i].entry->diskOffset + m_requests[i].offset <
                        m_requests[best].entry->diskOffset + m_requests[best].offset)
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
            bool ok = false;
            if (r.range)
            {
                blob.resize(r.bytes);
                ok = m_pak->ReadRawRange(r.entry, r.offset,
                    blob.empty() ? NULL : &blob[0], r.bytes);
                if (!ok) blob.clear();
            }
            else ok = m_pak->ReadBlob(r.entry, blob);
            QueryPerformanceCounter(&t1);

            if (!r.range)
            {
                const double ms = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)freq.QuadPart;
                const double mb = (double)r.entry->uncompressedSize / (1024.0 * 1024.0);
                char msg[160];
                sprintf_s(msg, sizeof(msg),
                          "cache: worker loaded 0x%08x  %u->%u bytes in %.2f ms (%.1f MB/s decoded)%s\n",
                          r.hash, r.entry->compressedSize, r.entry->uncompressedSize, ms,
                          ms > 0.0 ? mb / (ms / 1000.0) : 0.0, ok ? "" : "  [FAIL]");
                OutputDebugStringA(msg);
            }

            EnterCriticalSection(&m_compLock);
            m_completions.resize(m_completions.size() + 1);
            m_completions.back().hash = r.hash;
            m_completions.back().offset = r.offset;
            m_completions.back().bytes = r.bytes;
            m_completions.back().ok   = ok;
            m_completions.back().range = r.range;
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
    m_rangeResidentBytes = 0;
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
        if (it->second.mesh.skinVb) { it->second.mesh.skinVb->Release(); it->second.mesh.skinVb = NULL; }
        if (it->second.mesh.vb) { it->second.mesh.vb->Release(); it->second.mesh.vb = NULL; }
    }
    m_meshes.clear();
    for (std::map<unsigned int, CacheTex>::iterator it = m_textures.begin(); it != m_textures.end(); ++it)
        it->second.tex.Release();
    m_textures.clear();
    m_fonts.clear();
    m_ranges.clear();

    if (m_placeholder) { m_placeholder->Release(); m_placeholder = NULL; }
    m_requests.clear();
    m_completions.clear();
    m_residentBytes = 0;
    m_rangeResidentBytes = 0;
    m_device = NULL;
    m_pak    = NULL;
}

// ---------------------------------------------------------------------------
// Requests + render-thread completion drain
// ---------------------------------------------------------------------------
void StreamCache::Enqueue(unsigned int hash, const SpakEntry* entry)
{
    EnterCriticalSection(&m_reqLock);
    Request r; r.hash = hash; r.entry = entry; r.offset = r.bytes = 0; r.range = false;
    m_requests.push_back(r);
    LeaveCriticalSection(&m_reqLock);
    if (m_wake) SetEvent(m_wake);
}

void StreamCache::EnqueueRange(const RangeKey& key, const SpakEntry* entry)
{
    EnterCriticalSection(&m_reqLock);
    Request request;
    request.hash = key.hash;
    request.offset = key.offset;
    request.bytes = key.bytes;
    request.entry = entry;
    request.range = true;
    m_requests.push_back(request);
    LeaveCriticalSection(&m_reqLock);
    if (m_wake) SetEvent(m_wake);
}

bool StreamCache::GetRawRange(const SpakEntry* entry, unsigned int offset,
                              unsigned int bytes, void* out)
{
    if (!entry || spak::CodecOf(entry->flags) != spak::kCodecNone ||
        offset > entry->uncompressedSize || bytes > entry->uncompressedSize - offset)
        return false;
    if (bytes == 0) return true;

    const unsigned int requestedEnd = offset + bytes;
    bool ready = true;
    for (unsigned int pageOffset = (offset / spak::kStreamPageBytes) * spak::kStreamPageBytes;
         pageOffset < requestedEnd; pageOffset += spak::kStreamPageBytes)
    {
        RangeKey key;
        key.hash = entry->nameHash;
        key.offset = pageOffset;
        key.bytes = spak::kStreamPageBytes;
        if (key.bytes > entry->uncompressedSize - pageOffset)
            key.bytes = entry->uncompressedSize - pageOffset;
        std::map<RangeKey, RangePage>::iterator found = m_ranges.find(key);
        if (found == m_ranges.end())
        {
            RangePage page;
            page.lastUse = m_frame;
            m_ranges.insert(std::make_pair(key, page));
            EnqueueRange(key, entry);
            ready = false;
            continue;
        }
        found->second.lastUse = m_frame;
        if (found->second.state == StMissing)
        {
            found->second.state = StLoading;
            EnqueueRange(key, entry);
        }
        if (found->second.state != StResident || found->second.payload.size() != key.bytes)
            ready = false;
    }
    if (!ready) return false;

    if (out)
        for (unsigned int pageOffset = (offset / spak::kStreamPageBytes) * spak::kStreamPageBytes;
             pageOffset < requestedEnd; pageOffset += spak::kStreamPageBytes)
        {
            RangeKey key;
            key.hash = entry->nameHash;
            key.offset = pageOffset;
            key.bytes = spak::kStreamPageBytes;
            if (key.bytes > entry->uncompressedSize - pageOffset)
                key.bytes = entry->uncompressedSize - pageOffset;
            std::map<RangeKey, RangePage>::iterator found = m_ranges.find(key);
            const unsigned int copyStart = offset > pageOffset ? offset : pageOffset;
            const unsigned int pageEnd = pageOffset + key.bytes;
            const unsigned int copyEnd = requestedEnd < pageEnd ? requestedEnd : pageEnd;
            memcpy((unsigned char*)out + copyStart - offset,
                   &found->second.payload[copyStart - pageOffset], copyEnd - copyStart);
        }
    return true;
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
            c.offset = m_completions.back().offset;
            c.bytes = m_completions.back().bytes;
            c.ok   = m_completions.back().ok;
            c.range = m_completions.back().range;
            c.payload.swap(m_completions.back().payload);
            m_completions.pop_back();
            have = true;
        }
        LeaveCriticalSection(&m_compLock);
        if (!have)
            break;
        ++processed;

        if (c.range)
        {
            RangeKey key;
            key.hash = c.hash; key.offset = c.offset; key.bytes = c.bytes;
            std::map<RangeKey, RangePage>::iterator range = m_ranges.find(key);
            if (range != m_ranges.end())
            {
                if (c.ok && c.payload.size() == c.bytes)
                {
                    range->second.payload.swap(c.payload);
                    range->second.state = StResident;
                    range->second.lastUse = m_frame;
                    m_rangeResidentBytes += c.bytes;
                }
                else range->second.state = StMissing;
            }
            continue;
        }

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
            continue;
        }
        std::map<unsigned int, CacheFont>::iterator fit = m_fonts.find(c.hash);
        if (fit != m_fonts.end())
        {
            if (c.ok && !c.payload.empty())
                c.ok = text::ParseCookedFont(&c.payload[0], (unsigned int)c.payload.size(), fit->second.font);
            if (c.ok)
            {
                fit->second.state   = StResident;
                fit->second.bytes   = (unsigned int)c.payload.size();
                fit->second.lastUse = m_frame;
                m_residentBytes    += fit->second.bytes;
            }
            else fit->second.state = StMissing;
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

    while (m_rangeResidentBytes > m_rangeBudgetBytes)
    {
        std::map<RangeKey, RangePage>::iterator oldest = m_ranges.end();
        for (std::map<RangeKey, RangePage>::iterator it = m_ranges.begin(); it != m_ranges.end(); ++it)
            if (it->second.state == StResident &&
                it->second.lastUse + kEvictGraceFrames <= m_frame &&
                (oldest == m_ranges.end() || it->second.lastUse < oldest->second.lastUse))
                oldest = it;
        if (oldest == m_ranges.end()) break;
        m_rangeResidentBytes -= oldest->second.payload.size();
        m_ranges.erase(oldest);
    }
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
        // Find the coldest evictable resident asset across all maps.
        int   kind = -1;            // 0 = mesh, 1 = texture, 2 = font metadata
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

        for (std::map<unsigned int, CacheFont>::iterator it = m_fonts.begin(); it != m_fonts.end(); ++it)
            if (it->second.state == StResident && it->second.lastUse + kEvictGraceFrames <= m_frame &&
                it->second.lastUse < bestLast)
            { bestLast = it->second.lastUse; bestHash = it->first; kind = 2; }

        if (kind < 0)
            break; // everything resident is in active use — exceed budget gracefully

        unsigned int freed = 0;
        if (kind == 0)
        {
            std::map<unsigned int, CacheMesh>::iterator it = m_meshes.find(bestHash);
            if (it->second.mesh.ib) { it->second.mesh.ib->Release(); it->second.mesh.ib = NULL; }
            if (it->second.mesh.skinVb) { it->second.mesh.skinVb->Release(); it->second.mesh.skinVb = NULL; }
            if (it->second.mesh.vb) { it->second.mesh.vb->Release(); it->second.mesh.vb = NULL; }
            freed = it->second.bytes;
            m_meshes.erase(it); // next request re-enqueues it
        }
        else if (kind == 1)
        {
            std::map<unsigned int, CacheTex>::iterator it = m_textures.find(bestHash);
            it->second.tex.Release();
            freed = it->second.bytes;
            m_textures.erase(it);
        }
        else
        {
            std::map<unsigned int, CacheFont>::iterator it = m_fonts.find(bestHash);
            freed = it->second.bytes;
            m_fonts.erase(it);
        }
        m_residentBytes -= freed;

        char msg[128];
        sprintf_s(msg, sizeof(msg), "cache: evicted 0x%08x (%s), freed %u KB, resident %u MB / %u MB\n",
                  bestHash, kind == 0 ? "mesh" : kind == 1 ? "tex" : "font", freed / 1024,
                  (unsigned int)(m_residentBytes / (1024 * 1024)), (unsigned int)(m_budgetBytes / (1024 * 1024)));
        OutputDebugStringA(msg);
    }
}

void StreamCache::BuildMeshFromPayload(const std::vector<BYTE>& blob, CacheMesh& cm)
{
    if (blob.size() < spak::kMeshHeaderBytes)
        return;
    const unsigned char* p = &blob[0];
    const unsigned int magic = endian::LoadU32BE(p + 0);
    const bool skinned = magic == spak::kSkinMeshMagic;
    if (magic != spak::kMeshMagic && !skinned)
        return;
    const unsigned int headerBytes = skinned ? spak::kSkinMeshHeaderBytes : spak::kMeshHeaderBytes;
    if (blob.size() < headerBytes)
        return;
    const unsigned int vcount      = endian::LoadU32BE(p + 4);
    const unsigned int icount      = endian::LoadU32BE(p + 8);
    const unsigned int subsetCount = endian::LoadU32BE(p + 12);
    const unsigned int jointCount  = skinned ? endian::LoadU32BE(p + 16) : 0;
    const unsigned int fingerprint = skinned ? endian::LoadU32BE(p + 20) : 0;
    if (vcount == 0 || icount == 0 || subsetCount == 0 || subsetCount > 1024)
        return;
    if (skinned && (jointCount == 0 || jointCount > spak::kMaxSkinJoints))
        return;

    const size_t subBytes = (size_t)subsetCount * spak::kMeshSubsetBytes;
    const size_t vbytes   = (size_t)vcount * spak::kMeshVertexBytes;
    const size_t skinBytes = skinned ? (size_t)vcount * spak::kSkinInfluenceBytes : 0;
    const size_t ibytes   = (size_t)icount * 4;
    const size_t jointBytes = skinned ? (size_t)jointCount * spak::kSkinJointBytes : 0;
    if (headerBytes + subBytes + vbytes + skinBytes + ibytes + jointBytes > blob.size())
        return;

    // Per-material subsets: index range + alpha mode + texture hashes.
    cm.texHash.assign((size_t)subsetCount * spak::kMeshTexSlots, 0);
    for (unsigned int i = 0; i < subsetCount; ++i)
    {
        const unsigned char* sp = p + headerBytes + (size_t)i * spak::kMeshSubsetBytes;
        RtSubset sub;
        sub.indexStart = endian::LoadU32BE(sp + 0);
        sub.indexCount = endian::LoadU32BE(sp + 4);
        const unsigned int alpha = endian::LoadU32BE(sp + 8);
        const unsigned int kind  = alpha & spak::kAlphaKindMask;
        sub.alpha = (kind == spak::kAlphaCutout) ? RtCutout
                  : (kind == spak::kAlphaBlend)  ? RtBlend : RtOpaque;
        sub.normalHasHeight = (alpha & spak::kAlphaHeightBit) != 0;
        for (unsigned int s = 0; s < spak::kMeshTexSlots; ++s)
            cm.texHash[(size_t)i * spak::kMeshTexSlots + s] = endian::LoadU32BE(sp + 12 + s * 4);
        if ((size_t)sub.indexStart + sub.indexCount > icount)
        { cm.mesh.subsets.clear(); return; }
        cm.mesh.subsets.push_back(sub);
    }

    const size_t bufOff = headerBytes + subBytes;

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
    if (skinned)
    {
        if (FAILED(m_device->CreateVertexBuffer((UINT)skinBytes, 0, 0, D3DPOOL_MANAGED, &cm.mesh.skinVb, NULL)))
        {
            cm.mesh.vb->Release(); cm.mesh.vb = NULL;
            cm.mesh.subsets.clear();
            return;
        }
        void* dst = NULL;
        cm.mesh.skinVb->Lock(0, 0, &dst, 0);
        memcpy(dst, p + bufOff + vbytes, skinBytes);
        cm.mesh.skinVb->Unlock();
    }
    if (FAILED(m_device->CreateIndexBuffer((UINT)ibytes, 0, D3DFMT_INDEX32, D3DPOOL_MANAGED, &cm.mesh.ib, NULL)))
    {
        if (cm.mesh.skinVb) { cm.mesh.skinVb->Release(); cm.mesh.skinVb = NULL; }
        cm.mesh.vb->Release(); cm.mesh.vb = NULL;
        cm.mesh.subsets.clear();
        return;
    }
    {
        void* dst = NULL;
        cm.mesh.ib->Lock(0, 0, &dst, 0);
        memcpy(dst, p + bufOff + vbytes + skinBytes, ibytes);
        cm.mesh.ib->Unlock();
    }

    if (skinned)
    {
        const unsigned char* jp = p + bufOff + vbytes + skinBytes + ibytes;
        for (unsigned int i = 0; i < jointCount; ++i, jp += spak::kSkinJointBytes)
        {
            RtJoint joint;
            joint.nameHash = endian::LoadU32BE(jp + 0);
            joint.parent = (int)endian::LoadU32BE(jp + 4);
            if (joint.parent >= (int)i)
            {
                cm.mesh.Release();
                return;
            }
            memcpy(joint.inverseBind, jp + 8, 64);
            memcpy(joint.bindLocal, jp + 72, 64);
            cm.mesh.joints.push_back(joint);
        }
        cm.mesh.skeletonFingerprint = fingerprint;
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

IDirect3DTexture9* StreamCache::GetTexture(const std::string& relPath)
{
    return relPath.empty() ? NULL : GetTextureByHash(spak::NameHash(relPath.c_str()));
}

const text::CookedFont* StreamCache::GetFont(const std::string& relPath,
                                             IDirect3DTexture9** outAtlas)
{
    if (outAtlas) *outAtlas = NULL;
    if (relPath.empty() || !m_pak)
        return NULL;

    const unsigned int hash = spak::NameHash(relPath.c_str());
    std::map<unsigned int, CacheFont>::iterator it = m_fonts.find(hash);
    if (it == m_fonts.end())
    {
        CacheFont cached;
        const SpakEntry* entry = m_pak->Find(hash);
        cached.entry = entry;
        if (entry && entry->type == spak::kTypeFont)
        { cached.state = StLoading; m_fonts[hash] = cached; Enqueue(hash, entry); }
        else
        { cached.state = StMissing; m_fonts[hash] = cached; }
        it = m_fonts.find(hash);
    }

    CacheFont& cached = it->second;
    if (cached.state != StResident)
        return NULL;
    cached.lastUse = m_frame;
    IDirect3DTexture9* atlas = GetTextureByHash(cached.font.atlasHash);
    if (!atlas || atlas == m_placeholder)
        return NULL;
    if (outAtlas) *outAtlas = atlas;
    return &cached.font;
}

void StreamCache::RefreshMeshTextures(CacheMesh& cm)
{
    const size_t n = spak::kMeshTexSlots;
    for (size_t i = 0; i < cm.mesh.subsets.size() && i * n + (n - 1) < cm.texHash.size(); ++i)
    {
        RtSubset& s = cm.mesh.subsets[i];
        s.diffuse   = GetTextureByHash(cm.texHash[i * n + 0]);
        s.normal    = GetTextureByHash(cm.texHash[i * n + 1]);
        s.specular  = GetTextureByHash(cm.texHash[i * n + 2]);
        s.emissive  = GetTextureByHash(cm.texHash[i * n + 3]);
        s.metallic  = GetTextureByHash(cm.texHash[i * n + 4]);
        s.clearcoat = GetTextureByHash(cm.texHash[i * n + 5]);
        s.roughness = GetTextureByHash(cm.texHash[i * n + 6]);
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
