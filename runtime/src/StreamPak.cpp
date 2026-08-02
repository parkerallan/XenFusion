#include "StreamPak.h"
#include "Endian.h"

#include <xgraphics.h>  // XGOffsetBaseTextureAddress
#include <xcompress.h>  // XMemCreateDecompressionContext / XMemDecompress
#include <stdio.h>

// ---------------------------------------------------------------------------
// Local helpers
// ---------------------------------------------------------------------------
namespace
{
    void Log(const char* msg)
    {
        OutputDebugStringA("spak: ");
        OutputDebugStringA(msg);
        OutputDebugStringA("\n");
    }

    // The XPR2 magic, as it appears natively in the console's big-endian memory.
    const DWORD kXPR2Magic = 0x58505232; // 'XPR2'

    // XPR2 file header (see the XDK's AtgResource.cpp). Stored big-endian, i.e.
    // native on the console, so its DWORDs are read directly.
    struct XPR_HEADER
    {
        DWORD dwMagic;
        DWORD dwHeaderSize; // sysmem block size (resource headers)
        DWORD dwDataSize;   // vidmem block size (tiled GPU data)
    };

    // XPR2 resource tag (ATG RESOURCE). strName is stored as a sysmem offset; we
    // never resolve it here (lookup is by our own TOC hash), so it's unused.
    struct XPR_RESOURCE
    {
        DWORD dwType;
        DWORD dwOffset; // offset of this resource's header within the sysmem block
        DWORD dwSize;
        DWORD dwNameOffset;
    };

    // Caller serializes access because SetFilePointer + ReadFile is one logical
    // operation on the pak's shared persistent file handle.
    bool ReadFileAt(HANDLE f, unsigned int offset, void* dst, unsigned int bytes)
    {
        if (SetFilePointer(f, (LONG)offset, NULL, FILE_BEGIN) == INVALID_SET_FILE_POINTER)
            return false;
        DWORD got = 0;
        if (!ReadFile(f, dst, bytes, &got, NULL) || got != bytes)
            return false;
        return true;
    }
}

// ---------------------------------------------------------------------------
// StreamTexture
// ---------------------------------------------------------------------------
void StreamTexture::Release()
{
    // tex aliases sysMem — do not Release() it as a COM object.
    tex = NULL;
    if (vidMem) { XPhysicalFree(vidMem); vidMem = NULL; }
    if (sysMem) { delete[] sysMem;       sysMem = NULL; }
    vidBytes = 0;
    baseMinMip = baseMaxMip = 0;
}

// ---------------------------------------------------------------------------
// StreamPak
// ---------------------------------------------------------------------------
StreamPak::StreamPak()
    : m_file(INVALID_HANDLE_VALUE), m_toc(NULL), m_count(0), m_sectorSize(spak::kSectorSize)
{
    InitializeCriticalSection(&m_fileLock);
}

StreamPak::~StreamPak()
{
    Close();
    DeleteCriticalSection(&m_fileLock);
}

bool StreamPak::ReadAt(unsigned int offset, void* dst, unsigned int bytes)
{
    EnterCriticalSection(&m_fileLock);
    const bool ok = ReadFileAt(m_file, offset, dst, bytes);
    LeaveCriticalSection(&m_fileLock);
    return ok;
}

bool StreamPak::Size(const std::string&, unsigned int& bytes)
{
    if (m_file == INVALID_HANDLE_VALUE)
        return false;
    const DWORD size = GetFileSize(m_file, NULL);
    if (size == INVALID_FILE_SIZE || size == 0)
        return false;
    bytes = size;
    return true;
}

bool StreamPak::Read(const std::string&, unsigned int offset,
                     unsigned int length, std::vector<unsigned char>& out)
{
    out.clear();
    if (m_file == INVALID_HANDLE_VALUE)
        return false;
    if (length == 0)
    {
        unsigned int fileBytes = 0;
        if (!Size(std::string(), fileBytes) || offset > fileBytes)
            return false;
        length = fileBytes - offset;
    }
    out.resize(length);
    const bool ok = length == 0 || ReadAt(offset, &out[0], length);
    if (!ok)
        out.clear();
    return ok;
}

bool StreamPak::Open(const char* path)
{
    Close();

    m_file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (m_file == INVALID_HANDLE_VALUE)
    {
        Log("open failed (game.spak not present)");
        return false;
    }

    // Header (big-endian on disk).
    unsigned char hdr[spak::kHeaderBytes];
    if (!ReadAt(0, hdr, spak::kHeaderBytes))
    {
        Log("header read failed");
        Close();
        return false;
    }
    const unsigned int magic    = endian::LoadU32BE(hdr + 0);
    const unsigned int version  = endian::LoadU32BE(hdr + 4);
    const unsigned int count    = endian::LoadU32BE(hdr + 8);
    const unsigned int sector   = endian::LoadU32BE(hdr + 12);
    const unsigned int tocOff   = endian::LoadU32BE(hdr + 16);
    if (magic != spak::kMagic || version != spak::kVersion || count == 0)
    {
        Log("bad magic/version/empty");
        Close();
        return false;
    }
    m_sectorSize = sector ? sector : spak::kSectorSize;

    // TOC.
    const unsigned int tocBytes = count * spak::kEntryBytes;
    unsigned char* raw = new BYTE[tocBytes];
    if (!ReadAt(tocOff, raw, tocBytes))
    {
        Log("TOC read failed");
        delete[] raw;
        Close();
        return false;
    }
    m_toc = new SpakEntry[count];
    for (unsigned int i = 0; i < count; ++i)
    {
        const unsigned char* p = raw + i * spak::kEntryBytes;
        SpakEntry& e = m_toc[i];
        e.nameHash         = endian::LoadU32BE(p + 0);
        e.type             = endian::LoadU32BE(p + 4);
        e.flags            = endian::LoadU32BE(p + 8);
        e.diskOffset       = endian::LoadU32BE(p + 12);
        e.compressedSize   = endian::LoadU32BE(p + 16);
        e.uncompressedSize = endian::LoadU32BE(p + 20);
        e.sysMemSize       = endian::LoadU32BE(p + 24);
        e.vidMemSize       = endian::LoadU32BE(p + 28);
    }
    delete[] raw;
    m_count = count;

    {
        char msg[96];
        sprintf_s(msg, sizeof(msg), "opened, %u entr%s", m_count, m_count == 1 ? "y" : "ies");
        Log(msg);
    }
    return true;
}

void StreamPak::Close()
{
    if (m_toc) { delete[] m_toc; m_toc = NULL; }
    m_count = 0;
    if (m_file != INVALID_HANDLE_VALUE) { CloseHandle(m_file); m_file = INVALID_HANDLE_VALUE; }
}

const SpakEntry* StreamPak::At(unsigned int i) const
{
    return (i < m_count) ? &m_toc[i] : NULL;
}

const SpakEntry* StreamPak::Find(unsigned int nameHash) const
{
    // TOC is sorted ascending by nameHash.
    int lo = 0, hi = (int)m_count - 1;
    while (lo <= hi)
    {
        int mid = (lo + hi) >> 1;
        unsigned int h = m_toc[mid].nameHash;
        if (h == nameHash) return &m_toc[mid];
        if (h < nameHash)  lo = mid + 1;
        else               hi = mid - 1;
    }
    return NULL;
}

bool StreamPak::ReadBlob(const SpakEntry* e, std::vector<BYTE>& out)
{
    out.clear();
    if (m_file == INVALID_HANDLE_VALUE || e == NULL)
        return false;

    // Read the payload off disk. Pad the read buffer by 16 zero bytes:
    // XMemDecompress reads slightly past the end of its input, so the source must
    // have that slack or it fails intermittently.
    const unsigned int comp = e->compressedSize;
    std::vector<BYTE> readBuf(comp + 16, 0);
    if (comp && !ReadAt(e->diskOffset, &readBuf[0], comp))
    {
        Log("payload read failed");
        return false;
    }

    const unsigned int codec = spak::CodecOf(e->flags);
    if (codec == spak::kCodecLZX)
    {
        out.resize(e->uncompressedSize);
        XMEMDECOMPRESSION_CONTEXT ctx = NULL;
        if (FAILED(XMemCreateDecompressionContext(XMEMCODEC_LZX, NULL, 0, &ctx)))
        {
            Log("create decompress context failed");
            out.clear();
            return false;
        }
        SIZE_T dstSize = e->uncompressedSize;
        HRESULT hr = XMemDecompress(ctx, out.empty() ? NULL : &out[0], &dstSize,
                                    &readBuf[0], comp);
        XMemDestroyDecompressionContext(ctx);
        if (FAILED(hr) || dstSize != e->uncompressedSize)
        {
            Log("decompress failed");
            out.clear();
            return false;
        }
    }
    else // kCodecNone — the payload is the blob as-is
    {
        out.assign(readBuf.begin(), readBuf.begin() + comp);
    }
    return true;
}

bool StreamPak::ReadRawRange(const SpakEntry* e, unsigned int offset, void* out,
                             unsigned int bytes)
{
    if (m_file == INVALID_HANDLE_VALUE || e == NULL || out == NULL ||
        spak::CodecOf(e->flags) != spak::kCodecNone || offset > e->uncompressedSize ||
        bytes > e->uncompressedSize - offset)
        return false;
    return bytes == 0 || ReadAt(e->diskOffset + offset, out, bytes);
}

// Register an already-decompressed XPR2 blob into a live texture. No file I/O and
// no decompress, so the async worker can hand the blob over and this runs on the
// render thread (only CPU-memory / kernel-alloc work — no D3D device calls).
//
// `vidSrc`/`vidOffset`/`vidBytes` describe which slice of the vidmem block this
// blob actually carries. The allocation is always the full dwDataSize, so a
// partial load can be completed later by writing into the same memory — nothing
// ever moves, which is what keeps the GPU from ever seeing a stale pointer.
static bool RegisterXpr(const BYTE* xpr, unsigned int size,
                        const BYTE* vidSrc, DWORD vidOffset, DWORD vidBytes,
                        StreamTexture& out)
{
    out.Release();

    // --- Parse the XPR2 header (native big-endian on the console). ---
    if (xpr == NULL || size < sizeof(XPR_HEADER))
    {
        Log("XPR2 too small");
        return false;
    }
    const XPR_HEADER* xh = (const XPR_HEADER*)xpr;
    if (xh->dwMagic != kXPR2Magic)
    {
        Log("bad XPR2 magic");
        return false;
    }
    const DWORD headerSize = xh->dwHeaderSize;
    const DWORD dataSize   = xh->dwDataSize;
    if (sizeof(XPR_HEADER) + headerSize > size)
    {
        Log("XPR2 header overflows blob");
        return false;
    }
    if (vidOffset > dataSize || vidBytes > dataSize - vidOffset)
    {
        Log("XPR2 vidmem slice out of range");
        return false;
    }
    const unsigned char* sysSrc = xpr + sizeof(XPR_HEADER);

    // --- Split into the two runtime allocations: sysmem headers in a plain heap
    // block, vidmem tiled data in physically-contiguous write-combined memory the
    // GPU can read. ---
    BYTE* sysMem = new BYTE[headerSize];
    memcpy(sysMem, sysSrc, headerSize);

    void* vidMem = XPhysicalAlloc(dataSize, MAXULONG_PTR, 4096,
                                  PAGE_READWRITE | PAGE_WRITECOMBINE);
    if (vidMem == NULL)
    {
        Log("XPhysicalAlloc failed");
        delete[] sysMem;
        return false;
    }
    memcpy((BYTE*)vidMem + vidOffset, vidSrc, vidBytes);

    // --- Register: point the baked D3DTexture header at the real vidmem
    // allocation. The sysmem block is [numTags][RESOURCE tags...][headers...];
    // the texture header sits at tag.dwOffset within it. ---
    const DWORD numTags = *(const DWORD*)(sysMem + 0);
    if (numTags == 0)
    {
        Log("no resource tags in XPR2");
        XPhysicalFree(vidMem); delete[] sysMem;
        return false;
    }
    const XPR_RESOURCE* tags = (const XPR_RESOURCE*)(sysMem + 4);

    // Phase 1 bakes exactly one texture; take the first texture-typed tag.
    const XPR_RESOURCE* texTag = NULL;
    for (DWORD i = 0; i < numTags; ++i)
    {
        // ATG texture types share the high half-word ('TX' << 16).
        if ((tags[i].dwType & 0xffff0000) == (spak::kTypeTex2D & 0xffff0000))
        {
            texTag = &tags[i];
            break;
        }
    }
    if (texTag == NULL || texTag->dwOffset >= headerSize)
    {
        Log("no texture tag / bad offset");
        XPhysicalFree(vidMem); delete[] sysMem;
        return false;
    }

    IDirect3DTexture9* tex = (IDirect3DTexture9*)(sysMem + texTag->dwOffset);
    XGOffsetBaseTextureAddress(tex, vidMem, vidMem);

    out.tex        = tex;
    out.sysMem     = sysMem;
    out.vidMem     = vidMem;
    out.vidBytes   = dataSize;
    out.baseMinMip    = (unsigned char)tex->Format.MinMipLevel;
    out.baseMaxMip    = (unsigned char)tex->Format.MaxMipLevel;
    out.baseMipFilter = (unsigned char)tex->Format.MipFilter;
    return true;
}

bool StreamPak::RegisterTextureFromBlob(const BYTE* xpr, unsigned int size, StreamTexture& out)
{
    if (xpr == NULL || size < sizeof(XPR_HEADER)) { Log("XPR2 too small"); return false; }
    const XPR_HEADER* xh = (const XPR_HEADER*)xpr;
    const DWORD headerSize = xh->dwHeaderSize;
    const DWORD dataSize   = xh->dwDataSize;
    if (sizeof(XPR_HEADER) + headerSize + dataSize > size)
    { Log("XPR2 sizes overflow blob"); return false; }
    return RegisterXpr(xpr, size, xpr + sizeof(XPR_HEADER) + headerSize,
                       0, dataSize, out);
}

// 'TXLO': [magic | splitLevel | destOffset | byteCount][XPR2 header + sysmem][low mips]
bool StreamPak::RegisterTextureLo(const BYTE* payload, unsigned int size, StreamTexture& out)
{
    if (payload == NULL || size < spak::kTexLoPrefixBytes)
    { Log("TXLO payload too small"); return false; }
    const DWORD* p = (const DWORD*)payload;
    if (p[0] != spak::kTypeTexLo) { Log("bad TXLO magic"); return false; }
    const DWORD splitLevel = p[1];
    const DWORD destOffset = p[2];
    const DWORD byteCount  = p[3];

    const BYTE*  xpr     = payload + spak::kTexLoPrefixBytes;
    const unsigned int xprSize = size - spak::kTexLoPrefixBytes;
    if (xprSize < sizeof(XPR_HEADER)) { Log("TXLO xpr too small"); return false; }
    const XPR_HEADER* xh = (const XPR_HEADER*)xpr;
    const DWORD sysBytes = sizeof(XPR_HEADER) + xh->dwHeaderSize;
    if (xprSize < sysBytes || xprSize - sysBytes < byteCount)
    { Log("TXLO payload truncated"); return false; }

    if (!RegisterXpr(xpr, xprSize, xpr + sysBytes, destOffset, byteCount, out))
        return false;

    // Pin sampling to the one level we actually have. Nothing outside it can be
    // fetched, so the rest of the allocation is free to stay uninitialised — and
    // this is a per-texture header field, so no sampler state is touched and the
    // cutout/hair MIPFILTER behaviour is untouched.
    if (splitLevel > 15) { Log("TXLO split level out of range"); out.Release(); return false; }
    out.tex->Format.MinMipLevel = splitLevel;
    out.tex->Format.MaxMipLevel = splitLevel;
    // GPUMIPFILTER_BASEMAP means "ignore the chain and read level 0" — which is
    // exactly the level we have NOT loaded. Force POINT while clamped; with
    // Min == Max it resolves to the single level that is present.
    out.tex->Format.MipFilter = GPUMIPFILTER_POINT;
    return true;
}

// 'TXHI': [magic | destOffset | byteCount][mip 0 + levels coarser than the split]
bool StreamPak::ApplyTextureHi(IDirect3DDevice9* device, const BYTE* payload,
                               unsigned int size, StreamTexture& tex)
{
    if (tex.tex == NULL || tex.vidMem == NULL) { Log("TXHI with no base texture"); return false; }
    if (payload == NULL || size < spak::kTexHiPrefixBytes)
    { Log("TXHI payload too small"); return false; }
    const DWORD* p = (const DWORD*)payload;
    if (p[0] != spak::kTypeTexHi) { Log("bad TXHI magic"); return false; }
    const DWORD destOffset = p[1];
    const DWORD byteCount  = p[2];
    if (size - spak::kTexHiPrefixBytes < byteCount ||
        destOffset > tex.vidBytes || byteCount > tex.vidBytes - destOffset)
    { Log("TXHI slice out of range"); return false; }

    memcpy((BYTE*)tex.vidMem + destOffset, payload + spak::kTexHiPrefixBytes, byteCount);

    // vidmem is write-combined: flush the writes and drop any cached texels before
    // the clamp comes off and the GPU is allowed to read this region.
    if (device)
        device->InvalidateGpuCache(tex.vidMem, tex.vidBytes, 0);

    tex.tex->Format.MinMipLevel = tex.baseMinMip;
    tex.tex->Format.MaxMipLevel = tex.baseMaxMip;
    tex.tex->Format.MipFilter   = tex.baseMipFilter;
    return true;
}

bool StreamPak::LoadTextureSync(const SpakEntry* e, StreamTexture& out)
{
    if (e == NULL || e->type != spak::kTypeTex2D)
    {
        Log("entry is not a TX2D texture");
        return false;
    }
    std::vector<BYTE> blob;
    if (!ReadBlob(e, blob))
        return false;
    return RegisterTextureFromBlob(blob.empty() ? NULL : &blob[0], (unsigned int)blob.size(), out);
}
