#pragma once

#include <xtl.h>
#include <vector>
#include "SpakFormat.h"

// Phase 1 of the streaming subsystem (STREAMING.md): the console reader.
//
// Opens game.spak ONCE (one persistent handle — re-opening per asset is a DVD
// killer), holds the TOC, and can synchronously load + register a single
// texture entry into a live D3DTexture via the XPR2 pointer-fixup path. The
// async worker, residency/LRU, and meshes come in later phases; this proves the
// register path renders at all.

// One TOC entry, decoded from the big-endian on-disk record.
struct SpakEntry
{
    unsigned int nameHash;
    unsigned int type;
    unsigned int flags;
    unsigned int diskOffset;       // sector-aligned byte offset of the payload
    unsigned int compressedSize;   // payload bytes on disk
    unsigned int uncompressedSize; // XPR2 blob size after decompress
    unsigned int sysMemSize;       // XPR2 header block (resource headers)
    unsigned int vidMemSize;       // XPR2 data block (tiled GPU bytes)
};

// A registered texture. The D3DTexture header lives in sysMem; the tiled texels
// in physically-contiguous vidMem. `tex` ALIASES sysMem — it is not a COM object
// from CreateTexture, so never Release() it; call StreamTexture::Release to free
// the two backing allocations (which must outlive any use of `tex`).
struct StreamTexture
{
    IDirect3DTexture9* tex;
    BYTE*              sysMem; // new[]-allocated resource headers
    void*              vidMem; // XPhysicalAlloc'd tiled data (write-combined)

    StreamTexture() : tex(NULL), sysMem(NULL), vidMem(NULL) {}
    void Release();
};

class StreamPak
{
public:
    StreamPak();
    ~StreamPak();

    // Open "game:\\game.spak" (one handle, held until Close). Reads + validates
    // the header and loads the whole TOC. Returns false if absent/invalid.
    bool Open(const char* path);
    void Close();
    bool IsOpen() const { return m_file != INVALID_HANDLE_VALUE; }

    unsigned int     Count() const { return m_count; }
    const SpakEntry* At(unsigned int i) const;
    const SpakEntry* Find(unsigned int nameHash) const; // binary search on the TOC

    // Read the entry's payload off disk and decompress it (if flagged) into
    // `out`, sized to uncompressedSize. Shared by texture + mesh loads. Returns
    // false on I/O or decompress failure (logged).
    bool ReadBlob(const SpakEntry* e, std::vector<BYTE>& out);

    // Read a byte range from an uncompressed entry using the pak's persistent
    // handle. Used by ANIM track blocks so sampling never loads the whole
    // controller payload. Offset/size are relative to the entry payload.
    bool ReadRawRange(const SpakEntry* e, unsigned int offset, void* out,
                      unsigned int bytes);

    // Register an already-decompressed XPR2 blob (from ReadBlob) into a live
    // texture via pointer fixup. No I/O — safe to call on the render thread with a
    // blob the async worker produced. Returns false on a malformed blob.
    bool RegisterTextureFromBlob(const BYTE* xpr, unsigned int size, StreamTexture& out);

    // Synchronous texture load: ReadBlob + RegisterTextureFromBlob. Only TX2D
    // entries are valid. Returns false on failure.
    bool LoadTextureSync(const SpakEntry* e, StreamTexture& out);

private:
    bool ReadAt(unsigned int offset, void* dst, unsigned int bytes);

    HANDLE       m_file;
    SpakEntry*   m_toc;
    unsigned int m_count;
    unsigned int m_sectorSize;
    CRITICAL_SECTION m_fileLock;
};
