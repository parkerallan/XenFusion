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
bool StreamPak::RegisterTextureFromBlob(const BYTE* xpr, unsigned int size, StreamTexture& out)
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
    if (sizeof(XPR_HEADER) + headerSize + dataSize > size)
    {
        Log("XPR2 sizes overflow blob");
        return false;
    }
    const unsigned char* sysSrc = xpr + sizeof(XPR_HEADER);
    const unsigned char* vidSrc = sysSrc + headerSize;

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
    memcpy(vidMem, vidSrc, dataSize);

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

    out.tex    = tex;
    out.sysMem = sysMem;
    out.vidMem = vidMem;
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
