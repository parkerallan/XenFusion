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
    const unsigned int kStreamPageBytes = kSectorSize * 8; // 16 KiB ranged-I/O cache page

    const unsigned int kHeaderBytes = 20;         // 5 * u32
    const unsigned int kEntryBytes  = 32;         // 8 * u32

    // Resource-type fourccs. Texture payloads are XPR2 bundles (register by
    // pointer fixup); mesh payloads are our own {header + native VB + IB} blob
    // (phase 2 CreateBuffer + copy — see the mesh header below).
    const unsigned int kTypeTex2D = 0x54583244; // 'TX2D' — XPR2 texture
    const unsigned int kTypeMesh  = 0x4D455348; // 'MESH' — VB + IB + texture refs
    const unsigned int kTypeVideo = 0x56494445; // 'VIDE' — MPEG-1 program stream (.mpg),
                                                // ALWAYS stored raw (kCodecNone): the
                                                // VideoPlayer streams it with ranged
                                                // reads, which per-entry LZX would break
    const unsigned int kTypeAudio = 0x41554449; // 'AUDI' — raw MP2 elementary stream
                                                // (.mp2), ALWAYS stored raw: MP2 is
                                                // already compressed, and the AudioPlayer
                                                // reads the entry's byte window directly
    const unsigned int kTypeAnim  = 0x414E494D; // 'ANIM' — raw cooked controller +
                                                // independently addressable clip blocks
    const unsigned int kTypeFont  = 0x464F4E54; // 'FONT' — cooked glyph metrics +
                                                // reference to a TX2D SDF atlas

    // Cooked font payload (all fields big-endian):
    //   header: magic, version, atlasHash, atlasWidth, atlasHeight,
    //           sourcePixelSize, ascent, descent, lineHeight, sdfSpread,
    //           glyphCount, kerningCount
    //   glyph:  codepoint, advance, bearingX, bearingY, width, height, u0,v0,u1,v1
    //   kern:   leftCodepoint, rightCodepoint, advance
    // Scalar metrics and UVs are IEEE-754 f32 written as explicit BE words.
    const unsigned int kFontMagic       = 0x464E5431; // 'FNT1'
    const unsigned int kFontVersion     = 1;
    const unsigned int kFontHeaderBytes = 48;
    const unsigned int kFontGlyphBytes  = 40;
    const unsigned int kFontKernBytes   = 12;
    const unsigned int kMaxFontGlyphs   = 256;
    const unsigned int kMaxFontKerning  = 65536;

    // Static mesh payload (big-endian): a fixed 16-byte header, then one 40-byte record
    // per material subset, then vertexCount*44 bytes of native-endian vertices,
    // then indexCount*4 bytes of native-endian u32 indices. Each subset draws its
    // index range with its own textures (referenced by nameHash into TX2D
    // entries) and its own alphaKind.
    //   u32 magic 'MSH2' | vertexCount | indexCount | subsetCount
    //   per subset: u32 indexStart | indexCount | alphaKind | diffuseHash | normalHash | specHash | emissiveHash | metallicHash | clearcoatHash | roughnessHash
    const unsigned int kMeshMagic       = 0x4D534832; // 'MSH2'
    const unsigned int kMeshHeaderBytes = 16;         // 4 * u32
    const unsigned int kMeshSubsetBytes = 40;         // 10 * u32
    const unsigned int kMeshVertexBytes = 44;         // MeshVertex (pos/nrm/tan/uv)
    const unsigned int kMeshTexSlots    = 7;          // diffuse..roughness, in record order

    // Skinned mesh payload extends the same layout with an 8-byte influence
    // stream and fixed-size skeleton records:
    //   u32 magic 'MSH3' | vertexCount | indexCount | subsetCount |
    //       jointCount | skeletonFingerprint
    //   subsets | vertices | influences | indices | joints
    // Each influence is u8 joint[4] + u8 weight[4]. Each joint is nameHash,
    // signed parent, inverseBind[16], bindLocal[16], all big-endian except the
    // byte influence stream. Fixed records keep console loading allocation-free.
    const unsigned int kSkinMeshMagic       = 0x4D534833; // 'MSH3'
    const unsigned int kSkinMeshHeaderBytes = 24;         // 6 * u32
    const unsigned int kSkinInfluenceBytes  = 8;
    const unsigned int kSkinJointBytes      = 136;        // 2 * u32 + 32 floats
    const unsigned int kMaxSkinJoints       = 72;

    // alphaKind values (match RtAlphaKind: Opaque/Cutout/Blend). The low byte is
    // the kind; the byte above carries per-subset flags. The record length is
    // kMeshSubsetBytes above — the xex and the pak always deploy together, so
    // adding a texture slot needs no magic bump.
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
