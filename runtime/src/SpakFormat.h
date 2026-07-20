#pragma once

// SPAK — Xbox 360 asset stream container (see runtime/STREAMING.md).
//
// One file, opened once with a single persistent handle; a TOC sorted by
// nameHash for binary-search lookup; each entry's payload is a whole XPR2
// bundle (produced offline by the XDK Bundler), optionally LZX-compressed as a
// unit. At load the runtime reads the payload, decompresses it, and registers
// the texture by pointer fixup (XGOffsetBaseTextureAddress) — no CreateTexture,
// no pixel copy. See StreamPak (console) and spakc (PC cooker).
//
// All multi-byte fields are BIG-ENDIAN on disk: the cooker (little-endian PC)
// writes them byte-swapped; the console (big-endian PowerPC) reads them
// natively. This header is shared by both, so it stays free of platform headers.
//
// On-disk layout:
//   [Header: 20 bytes]  magic|version|entryCount|sectorSize|tocOffset
//   [TOC: entryCount * 32 bytes]  sorted by nameHash
//   [pad to sectorSize]
//   [Data: per entry, sector-aligned, {compressed | raw} XPR2 payload]

namespace spak
{
    const unsigned int kMagic       = 0x5350414B; // 'SPAK'
    const unsigned int kVersion     = 1;
    const unsigned int kSectorSize  = 2048;       // DVD sector; data is aligned to it

    const unsigned int kHeaderBytes = 20;         // 5 * u32
    const unsigned int kEntryBytes  = 32;         // 8 * u32

    // Resource-type fourccs. Texture payloads are XPR2 bundles (register by
    // pointer fixup); mesh payloads are our own {header + native VB + IB} blob
    // (phase 2 CreateBuffer + copy — see the mesh header below).
    const unsigned int kTypeTex2D = 0x54583244; // 'TX2D' — XPR2 texture
    const unsigned int kTypeMesh  = 0x4D455348; // 'MESH' — VB + IB + texture refs

    // Mesh payload (big-endian): a fixed 16-byte header, then one 28-byte record
    // per material subset, then vertexCount*44 bytes of native-endian vertices,
    // then indexCount*4 bytes of native-endian u32 indices. Each subset draws its
    // index range with its own textures (referenced by nameHash into TX2D
    // entries) and its own alphaKind.
    //   u32 magic 'MSH2' | vertexCount | indexCount | subsetCount
    //   per subset: u32 indexStart | indexCount | alphaKind | diffuseHash | normalHash | specHash | emissiveHash
    const unsigned int kMeshMagic       = 0x4D534832; // 'MSH2'
    const unsigned int kMeshHeaderBytes = 16;         // 4 * u32
    const unsigned int kMeshSubsetBytes = 28;         // 7 * u32
    const unsigned int kMeshVertexBytes = 44;         // MeshVertex (pos/nrm/tan/uv)

    // alphaKind values (match RtAlphaKind: Opaque/Cutout/Blend). The low byte is
    // the kind; the byte above carries per-subset flags (record stays 24 bytes —
    // the xex and the pak always deploy together, so no magic bump).
    const unsigned int kAlphaOpaque = 0;
    const unsigned int kAlphaCutout = 1;
    const unsigned int kAlphaBlend  = 2;
    const unsigned int kAlphaKindMask  = 0xFFu;
    const unsigned int kAlphaHeightBit = 0x100u; // normal map's alpha carries a
                                                 // height field (0.5 = neutral)

    // flags word: bit0 = payload is compressed; bits1..3 = codec id (below).
    const unsigned int kFlagCompressed = 0x1u;
    const unsigned int kCodecShift     = 1;
    const unsigned int kCodecMask      = 0x7u << 1;
    const unsigned int kCodecNone      = 0;
    const unsigned int kCodecLZX       = 1; // XMemCompress/XMemDecompress

    inline unsigned int MakeFlags(unsigned int codec)
    {
        if (codec == kCodecNone) return 0;
        return kFlagCompressed | ((codec << kCodecShift) & kCodecMask);
    }
    inline unsigned int CodecOf(unsigned int flags)
    {
        return (flags & kFlagCompressed) ? ((flags & kCodecMask) >> kCodecShift) : kCodecNone;
    }

    // FNV-1a (32-bit) of a relative asset path, normalised: '\\' -> '/', lower-cased.
    // Both the cooker and the runtime hash the same way so lookups match.
    inline unsigned int NameHash(const char* s)
    {
        unsigned int h = 2166136261u;
        for (; s && *s; ++s)
        {
            char c = *s;
            if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
            if (c == '\\')            c = '/';
            h ^= (unsigned char)c;
            h *= 16777619u;
        }
        return h;
    }
}
