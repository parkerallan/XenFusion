//--------------------------------------------------------------------------------------
// spakc — Xbox 360 asset stream cooker.
//
// Modes:
//   spakc build <out.spak> <contentRoot> <assets...> [--image <imageRel>] [--raw]
//       Cook a multi-entry game.spak from a set of meshes. Each mesh's M360 blob
//       becomes a MESH entry (native big-endian VB/IB + baked alpha kind + the
//       nameHashes of its 3 textures); each referenced texture is baked to an
//       XPR2 TX2D entry via the XDK Bundler (deduped across meshes). meshRelN are
//       the scene-relative paths the runtime requests (e.g. assets/models/x.mesh).
//
//   spakc tex <source-image> <logical-name> <out.spak> [--raw]
//       Cook a single texture into a one-entry .spak (ad-hoc / probe).
//
// Textures are baked by Bundler.exe (found via XEDK); meshes and LZX compression
// (XMemCompress) are done here. Every entry is LZX-compressed as a unit unless
// --raw. Self-verifies each compressed entry by decompressing it back.
//--------------------------------------------------------------------------------------
#include <windows.h>
#include <xcompress.h>
#include <d3d9.h>      // XDK win32 D3D types (D3DFORMAT, IDirect3DTexture9 header)
#include <xgraphics.h> // XGSetTextureHeader / XGGetMipLevelOffset — mip layout math
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string>
#include <vector>
#include <set>
#include <algorithm>
#include <math.h>

#include "SpakFormat.h"
#include "anim/FaceClip.h" // shared magic, so the cook cannot drift
#include "image/GifAnim.h" // shared .gif detection, so routing matches both renderers

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

namespace
{
// ---- byte helpers ---------------------------------------------------------
unsigned int ReadU32LE(const unsigned char* p)
{
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8) |
           ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
}
unsigned int ReadU32BE(const unsigned char* p)
{
    return ((unsigned int)p[0] << 24) | ((unsigned int)p[1] << 16) |
           ((unsigned int)p[2] << 8)  |  (unsigned int)p[3];
}
void PushU32BE(std::vector<unsigned char>& v, unsigned int u)
{
    v.push_back((unsigned char)((u >> 24) & 0xFF));
    v.push_back((unsigned char)((u >> 16) & 0xFF));
    v.push_back((unsigned char)((u >> 8)  & 0xFF));
    v.push_back((unsigned char)( u        & 0xFF));
}
void PushU32LE(std::vector<unsigned char>& v, unsigned int u)
{
    v.push_back((unsigned char)( u        & 0xFF));
    v.push_back((unsigned char)((u >> 8)  & 0xFF));
    v.push_back((unsigned char)((u >> 16) & 0xFF));
    v.push_back((unsigned char)((u >> 24) & 0xFF));
}
void PushF32BE(std::vector<unsigned char>& v, float value)
{
    unsigned int bits = 0;
    memcpy(&bits, &value, sizeof(bits));
    PushU32BE(v, bits);
}
// Append `src` (little-endian 32-bit words) byte-reversed, i.e. as big-endian —
// the console reads VB/IB natively (big-endian), so buffers are baked that way.
void PushSwappedWords(std::vector<unsigned char>& v, const unsigned char* src, size_t bytes)
{
    for (size_t k = 0; k + 4 <= bytes; k += 4)
    {
        v.push_back(src[k + 3]);
        v.push_back(src[k + 2]);
        v.push_back(src[k + 1]);
        v.push_back(src[k + 0]);
    }
}

bool ReadFileBytes(const std::string& path, std::vector<unsigned char>& out)
{
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return false; }
    out.resize((size_t)n);
    size_t got = fread(&out[0], 1, (size_t)n, f);
    fclose(f);
    return got == (size_t)n;
}
bool WriteFileBytes(const std::string& path, const unsigned char* data, size_t bytes)
{
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    size_t put = fwrite(data, 1, bytes, f);
    fclose(f);
    return put == bytes;
}

// Write RGBA8 pixels (stb's order) as an uncompressed 32-bit top-left TGA, the
// intermediate Bundler reads. `rgba` is width*height*4 bytes.
bool WriteRgbaTga(const std::string& path, const unsigned char* rgba,
                  int width, int height)
{
    if (width <= 0 || height <= 0 || rgba == NULL)
        return false;
    std::vector<unsigned char> tga(18 + (size_t)width * height * 4, 0);
    tga[2] = 2; // uncompressed true-color
    tga[12] = (unsigned char)(width & 0xFF);
    tga[13] = (unsigned char)((width >> 8) & 0xFF);
    tga[14] = (unsigned char)(height & 0xFF);
    tga[15] = (unsigned char)((height >> 8) & 0xFF);
    tga[16] = 32;
    tga[17] = 0x28; // 8 alpha bits, top-left origin
    for (int i = 0; i < width * height; ++i)
    {
        tga[18 + i * 4 + 0] = rgba[i * 4 + 2]; // B
        tga[18 + i * 4 + 1] = rgba[i * 4 + 1]; // G
        tga[18 + i * 4 + 2] = rgba[i * 4 + 0]; // R
        tga[18 + i * 4 + 3] = rgba[i * 4 + 3]; // A
    }
    return WriteFileBytes(path, &tga[0], tga.size());
}

// An SDF glyph atlas: coverage lives in alpha, RGB is unused and written white.
bool WriteAtlasTga(const std::string& path, const std::vector<unsigned char>& alpha,
                   int width, int height)
{
    if (width <= 0 || height <= 0 || alpha.size() != (size_t)width * height)
        return false;
    std::vector<unsigned char> rgba((size_t)width * height * 4, 255);
    for (int i = 0; i < width * height; ++i)
        rgba[i * 4 + 3] = alpha[i];
    return WriteRgbaTga(path, &rgba[0], width, height);
}

// Normalise a path (absolute or relative-to-CWD) to a full path.
std::string AbsPath(const std::string& path)
{
    char buf[MAX_PATH];
    DWORD n = GetFullPathNameA(path.c_str(), MAX_PATH, buf, NULL);
    return (n > 0 && n < MAX_PATH) ? std::string(buf) : path;
}

// Resolve `rel` (possibly with ..) against `baseAbs` into a normalised absolute path.
std::string AbsFrom(const std::string& baseAbs, const std::string& rel)
{
    std::string combined = baseAbs;
    if (!combined.empty() && combined[combined.size() - 1] != '\\' && combined[combined.size() - 1] != '/')
        combined += "\\";
    for (size_t i = 0; i < rel.size(); ++i)
        combined += (rel[i] == '/') ? '\\' : rel[i];
    char buf[MAX_PATH];
    DWORD n = GetFullPathNameA(combined.c_str(), MAX_PATH, buf, NULL);
    return (n > 0 && n < MAX_PATH) ? std::string(buf) : combined;
}
std::string DirOf(const std::string& path)
{
    size_t s = path.find_last_of("\\/");
    return (s == std::string::npos) ? std::string() : path.substr(0, s);
}

// ---- compression ----------------------------------------------------------
size_t LzxCompress(const unsigned char* src, size_t srcSize, std::vector<unsigned char>& dst)
{
    XMEMCOMPRESSION_CONTEXT ctx = NULL;
    if (FAILED(XMemCreateCompressionContext(XMEMCODEC_LZX, NULL, 0, &ctx)))
    { fprintf(stderr, "spakc: XMemCreateCompressionContext failed\n"); return 0; }
    dst.resize(srcSize + srcSize / 2 + 65536);
    SIZE_T dstSize = dst.size();
    HRESULT hr = XMemCompress(ctx, &dst[0], &dstSize, src, srcSize);
    XMemDestroyCompressionContext(ctx);
    if (FAILED(hr)) { fprintf(stderr, "spakc: XMemCompress failed (0x%08lx)\n", hr); return 0; }
    dst.resize(dstSize);
    return dstSize;
}
bool LzxVerify(const std::vector<unsigned char>& comp, unsigned int uncompressedSize)
{
    std::vector<unsigned char> in = comp;
    in.resize(in.size() + 16, 0);
    std::vector<unsigned char> out(uncompressedSize);
    XMEMDECOMPRESSION_CONTEXT ctx = NULL;
    if (FAILED(XMemCreateDecompressionContext(XMEMCODEC_LZX, NULL, 0, &ctx))) return false;
    SIZE_T dstSize = uncompressedSize;
    HRESULT hr = XMemDecompress(ctx, out.empty() ? NULL : &out[0], &dstSize,
                                &in[0], comp.size());
    XMemDestroyDecompressionContext(ctx);
    return SUCCEEDED(hr) && dstSize == uncompressedSize;
}

// ---- Bundler --------------------------------------------------------------
std::string g_bundler;
std::string g_tmpBase; // temp file base (out.spak path)

bool RunBundler(const std::string& rdf, const std::string& xpr)
{
    std::string cmd = "\"" + g_bundler + "\" \"" + rdf + "\" -o \"" + xpr + "\"";
    std::vector<char> cl(cmd.begin(), cmd.end()); cl.push_back('\0');
    STARTUPINFOA si; ZeroMemory(&si, sizeof(si)); si.cb = sizeof(si);
    PROCESS_INFORMATION pi; ZeroMemory(&pi, sizeof(pi));
    if (!CreateProcessA(NULL, &cl[0], NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi))
    { fprintf(stderr, "spakc: failed to launch Bundler (%lu)\n", GetLastError()); return false; }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1; GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    if (code != 0) fprintf(stderr, "spakc: Bundler exited %lu\n", code);
    return code == 0;
}

// Bake images to an XPR2 blob via Bundler. `format` is a D3DFMT_* name (DXT5
// for diffuse; A8R8G8B8 uncompressed for normal maps, whose smooth gradients DXT
// facets into visible block artifacts). Returns false on failure.
//
// One source cooks a plain <Texture>. Several cook an <ArrayTexture>: one Xenos
// stacked texture with a slice per source, each with its own mip chain. That is
// how an animated GIF ships — see SpakFormat.h's 'GIFA' block.
//
// Callers pass the source strings EXACTLY as they should appear in the RDF.
// That matters for the array form: Bundler rejects any element whose tag text
// exceeds 2048 characters (error 861b0002), which 64 absolute paths blow
// through on their own. It resolves a relative Source against the RDF's own
// directory, so the array caller writes its temp frames beside g_tmpBase and
// passes bare filenames — 64 of those is about 1150 characters.
bool CookTextureXPR(const std::vector<std::string>& sources, const std::string& name,
                    bool sRGB, const char* format, std::vector<unsigned char>& xprOut)
{
    if (sources.empty()) return false;
    const std::string rdfPath = g_tmpBase + ".tmp.rdf";
    const std::string xprPath = g_tmpBase + ".tmp.xpr";
    const std::string element = sources.size() == 1 ? "Texture" : "ArrayTexture";
    std::string rdf =
        "<?xml version=\"1.0\" encoding=\"utf-8\" ?>\n<RDF Version=\"XPR2\">\n"
        "  <" + element + " Name=\"" + name + "\"";
    for (size_t i = 0; i < sources.size(); ++i)
        rdf += " Source=\"" + sources[i] + "\"";
    rdf += std::string(" Format=\"") + format + "\" Levels=\"0\" sRGB=\"" +
           (sRGB ? "true" : "false") + "\" />\n</RDF>\n";
    if (!WriteFileBytes(rdfPath, (const unsigned char*)rdf.data(), rdf.size())) return false;
    bool ok = RunBundler(rdfPath, xprPath) && ReadFileBytes(xprPath, xprOut) && xprOut.size() >= 12
              && ReadU32BE(&xprOut[0]) == 0x58505232u;
    DeleteFileA(rdfPath.c_str());
    DeleteFileA(xprPath.c_str());
    return ok;
}

// Single-source form: every existing caller.
bool CookTextureXPR(const std::string& absSource, const std::string& name, bool sRGB,
                    const char* format, std::vector<unsigned char>& xprOut)
{
    return CookTextureXPR(std::vector<std::string>(1, absSource), name, sRGB, format, xprOut);
}

// ---- 360 mip layout -------------------------------------------------------
// The console's texture header addresses mip 0 and mips 1..N independently, so a
// texture can be made renderable from just its small mips while the rest is still
// streaming. Those offsets are a pure function of (width, height, levels, format),
// so we compute them with the same XG library Bundler uses rather than parsing
// its big-endian output — no endian surgery, no dependency on XPR2 internals.

// Number of mip levels in a full chain (Levels="0" in the RDF), down to 1x1.
unsigned int FullMipLevels(int width, int height)
{
    unsigned int levels = 1;
    int w = width, h = height;
    while (w > 1 || h > 1)
    {
        w = (w > 1) ? w / 2 : 1;
        h = (h > 1) ? h / 2 : 1;
        ++levels;
    }
    return levels;
}

struct MipSplit
{
    unsigned int levels;      // full chain length
    unsigned int splitLevel;  // first level carried by the low chunk
    unsigned int lowOffset;   // byte offset of the low chunk within the vidmem block
    unsigned int totalSize;   // baseSize + mipSize; must equal the XPR2's dwDataSize
    unsigned int splitWidth;  // pixel size of splitLevel, for logging
    unsigned int splitHeight;
};

// Longest side we want the first-visible level to be. Small enough to read as
// "still loading", large enough to show the material rather than a flat colour.
const unsigned int kSplitMaxSide = 32;

// Only split when the low chunk is at most this fraction of the whole — otherwise
// the second entry buys nothing and just costs a seek.
//
// Tile padding puts a ~16 KB floor under EVERY mip level (a 32x32, 64x64 and
// 128x128 DXT5 all occupy 16384 bytes), so the low chunk is a fixed ~32 KB — the
// 32x32 level plus the packed tail — no matter how large the parent texture is.
// The threshold is therefore really a statement about the smallest texture worth
// splitting: at 1/2, that is 128x128 (64 KB, exactly half). Anything smaller has
// nothing to save, since the whole texture is already near the floor.
//
// Deliberately NOT stepping to coarser levels when this fails: the first visible
// level is then always 32x32 for everything that splits at all, which is a rule
// you can hold in your head. Allowing a step-down would make 64x64 textures open
// at 16x16 and reintroduce exactly the inconsistency this avoids.
const unsigned int kSplitMaxLowFraction = 2; // i.e. low <= 1/2 of total

bool ComputeMipSplit(int width, int height, D3DFORMAT format, MipSplit& out)
{
    if (width <= 0 || height <= 0) return false;

    const unsigned int levels = FullMipLevels(width, height);
    if (levels < 2) return false; // nothing to split

    IDirect3DTexture9 header;
    ZeroMemory(&header, sizeof(header));
    UINT baseSize = 0, mipSize = 0;
    XGSetTextureHeader((UINT)width, (UINT)height, levels, 0, format, D3DPOOL_DEFAULT,
                       0, XGHEADER_CONTIGUOUS_MIP_OFFSET, 0, &header, &baseSize, &mipSize);
    if (baseSize == 0 || mipSize == 0) return false;

    // Level whose longest side has dropped to kSplitMaxSide — the level we would
    // LIKE to be the first one visible.
    unsigned int kSize = 0;
    { int w = width, h = height;
      while (((unsigned)w > kSplitMaxSide || (unsigned)h > kSplitMaxSide) && kSize + 1 < levels)
      { ++kSize; w = (w > 1) ? w / 2 : 1; h = (h > 1) ? h / 2 : 1; } }
    // kSize == 0 means the texture is ALREADY 32x32 or smaller, so mip 0 is the
    // first look. Splitting would only make it open blurrier than its own full
    // resolution, which is worse than not splitting at all.
    if (kSize == 0) return false;

    const unsigned int total = baseSize + mipSize;
    const unsigned int k     = kSize;

    // Start of the slice = the LOWEST offset among every level we are keeping. This
    // is NOT the same as offset(k): levels from the packed-tail base share one tile
    // and their offsets run BACKWARDS (128x128: L3=32832 but L4=32800, L5=32784).
    // Taking offset(k) there would cut off finer levels and corrupt them.
    // Over-including a few bytes is harmless — the high chunk covers the remainder,
    // and together they still tile the block exactly once.
    UINT offK = XGGetMipLevelOffset(&header, 0, k);
    for (unsigned int L = k + 1; L < levels; ++L)
    {
        const UINT o = XGGetMipLevelOffset(&header, 0, L);
        if (o < offK) offK = o;
    }
    if (offK >= mipSize) return false; // not relative to the mip block — bail loudly

    const unsigned int lowBytes = mipSize - offK;
    if (lowBytes * kSplitMaxLowFraction > total)
        return false; // too much of the texture to be worth a second entry

    out.levels     = levels;
    out.splitLevel = k;
    out.lowOffset  = baseSize + offK;
    out.totalSize  = total;
    { int w = width, h = height;
      for (unsigned int i = 0; i < k; ++i) { w = (w > 1) ? w / 2 : 1; h = (h > 1) ? h / 2 : 1; }
      out.splitWidth = (unsigned)w; out.splitHeight = (unsigned)h; }
    return true;
}

// Classify the diffuse alpha like the editor: no alpha -> Opaque; >~3% fully
// transparent texels (holes) -> Cutout; otherwise translucent -> Blend.
unsigned int ClassifyAlphaRgba(const unsigned char* rgba, int pixelCount)
{
    if (!rgba || pixelCount <= 0) return spak::kAlphaOpaque;
    int translucent = 0, transparent = 0;
    for (int i = 0; i < pixelCount; ++i)
    {
        unsigned char a = rgba[i * 4 + 3];
        if (a < 255) ++translucent;
        if (a < 24)  ++transparent;
    }
    if (translucent == 0)                   return spak::kAlphaOpaque;
    if (transparent > pixelCount / 32)      return spak::kAlphaCutout;
    return spak::kAlphaBlend;
}

unsigned int ClassifyAlpha(const std::string& absPng)
{
    int w = 0, h = 0, ch = 0;
    unsigned char* px = stbi_load(absPng.c_str(), &w, &h, &ch, 4);
    if (!px) return spak::kAlphaOpaque;
    const unsigned int kind = ClassifyAlphaRgba(px, w * h);
    stbi_image_free(px);
    return kind;
}

// Does a normal map's alpha carry a height field (bump offset, 0.5 = neutral)?
// Mirrors the editor's check: only if the alpha actually varies — a normal map
// without an alpha channel (stb reports the file's channel count) or with a
// blank one carries no relief.
bool NormalAlphaHasHeight(const std::string& absPng)
{
    int w = 0, h = 0, ch = 0;
    unsigned char* px = stbi_load(absPng.c_str(), &w, &h, &ch, 4);
    if (!px) return false;
    bool varies = false;
    if (ch == 4)
    {
        unsigned char amin = 255, amax = 0;
        const int total = w * h;
        for (int i = 0; i < total; ++i)
        {
            unsigned char a = px[i * 4 + 3];
            if (a < amin) amin = a;
            if (a > amax) amax = a;
        }
        varies = (amax - amin) >= 8;
    }
    stbi_image_free(px);
    return varies;
}

// ---- entries + writer -----------------------------------------------------

// Disk-placement groups. The TOC stays sorted by hash for binary search, but disk
// offsets are assigned group by group so the blurry pass is one forward march
// across the disc instead of seeking wherever hash order happens to scatter it.
// (STREAMING.md always specified load-sequence order; until now it wasn't done.)
enum DiskGroup
{
    kGroupLow  = 0, // TXLO — every texture's small mips, read first
    kGroupMain = 1, // meshes, fonts, unsplit textures, media
    kGroupHigh = 2  // TXHI — full-resolution data, read last
};

struct Entry
{
    unsigned int hash;
    unsigned int type;
    std::vector<unsigned char> payload; // uncompressed
    unsigned int sysMemSize;
    unsigned int vidMemSize;
    bool         noCompress; // video: stored raw so the runtime can range-read it
    int          diskGroup;

    Entry() : hash(0), type(0), sysMemSize(0), vidMemSize(0), noCompress(false),
              diskGroup(kGroupMain) {}
};

bool EntryLess(const Entry& a, const Entry& b) { return a.hash < b.hash; }

D3DFORMAT D3DFormatFromName(const char* name)
{
    if (!name) return (D3DFORMAT)0;
    if (strcmp(name, "D3DFMT_DXT1")     == 0) return D3DFMT_DXT1;
    if (strcmp(name, "D3DFMT_DXT5")     == 0) return D3DFMT_DXT5;
    if (strcmp(name, "D3DFMT_A8R8G8B8") == 0) return D3DFMT_A8R8G8B8;
    if (strcmp(name, "D3DFMT_A8")       == 0) return D3DFMT_A8;
    return (D3DFORMAT)0; // unknown -> don't split
}

// Cook + add one STACKED (array) texture entry: `sources` become slices, in
// order. Emitted as an ordinary kTypeTex2D entry because Bundler tags an array
// texture RESOURCETYPE_TEXTURE exactly like a 2D one, so the runtime registers
// it through the unchanged path; only the draw knows it is stacked.
//
// Never split — TXLO/TXHI carves a mip chain out of one 2D surface and has no
// meaning for a stack. Sources are emitted into the RDF verbatim; see
// CookTextureXPR for why they must be bare filenames beside g_tmpBase.
unsigned int AddArrayTexture(std::vector<Entry>& entries, std::set<unsigned int>& seen,
                             const std::vector<std::string>& sources, const std::string& name,
                             unsigned int hash, bool sRGB, const char* format)
{
    if (sources.empty() || hash == 0) return 0;
    if (seen.find(hash) != seen.end()) return hash; // already cooked
    std::vector<unsigned char> xpr;
    if (!CookTextureXPR(sources, name, sRGB, format, xpr))
    { fprintf(stderr, "spakc: WARN array texture cook failed: %s\n", name.c_str()); return 0; }

    Entry e;
    e.hash = hash; e.type = spak::kTypeTex2D; e.payload = xpr;
    e.sysMemSize = ReadU32BE(&xpr[4]);
    e.vidMemSize = ReadU32BE(&xpr[8]);
    entries.push_back(e);
    seen.insert(hash);
    return hash;
}

// Cook + add one texture entry, deduped by hash. `absSource` feeds Bundler;
// `hash` is the internal link key (mesh -> texture). Returns the hash (0 if the
// slot is empty or cook failed, so the runtime falls back to a default).
//
// Large textures are emitted as a TXLO/TXHI pair so they can be drawn blurry
// while the full-resolution data is still streaming; see SpakFormat.h. Pass
// allowSplit=false for textures where a blurry stage is meaningless (SDF atlases).
unsigned int AddTexture(std::vector<Entry>& entries, std::set<unsigned int>& seen,
                        const std::string& absSource, const std::string& name,
                        unsigned int hash, bool sRGB, const char* format,
                        bool allowSplit = true)
{
    if (absSource.empty() || hash == 0) return 0;
    if (seen.find(hash) != seen.end()) return hash; // already cooked
    std::vector<unsigned char> xpr;
    if (!CookTextureXPR(absSource, name, sRGB, format, xpr))
    { fprintf(stderr, "spakc: WARN texture cook failed: %s\n", absSource.c_str()); return 0; }

    const unsigned int kXprHeaderBytes = 12; // magic | dwHeaderSize | dwDataSize
    const unsigned int sysMemSize = ReadU32BE(&xpr[4]);
    const unsigned int vidMemSize = ReadU32BE(&xpr[8]);

    // Decide whether to split. Every failure path here falls back to the original
    // single-entry form, so a texture is never at risk of shipping corrupt.
    MipSplit split;
    bool doSplit = false;
    if (allowSplit && xpr.size() >= kXprHeaderBytes + sysMemSize + vidMemSize)
    {
        const D3DFORMAT fmt = D3DFormatFromName(format);
        int w = 0, h = 0, comp = 0;
        if (fmt != (D3DFORMAT)0 && stbi_info(absSource.c_str(), &w, &h, &comp) &&
            ComputeMipSplit(w, h, fmt, split))
        {
            // The one check that matters: our locally computed layout must agree
            // with what Bundler actually emitted. If it does, the offsets are right
            // by construction; if not, ship unsplit rather than guess.
            if (split.totalSize == vidMemSize)
                doSplit = true;
            else
                fprintf(stderr, "spakc: WARN mip layout mismatch for %s "
                                "(computed %u, Bundler %u) — shipping unsplit\n",
                        absSource.c_str(), split.totalSize, vidMemSize);
        }
    }

    if (!doSplit)
    {
        Entry e;
        e.hash = hash; e.type = spak::kTypeTex2D; e.payload = xpr;
        e.sysMemSize = sysMemSize; e.vidMemSize = vidMemSize;
        entries.push_back(e);
        seen.insert(hash);
        return hash;
    }

    const unsigned char* vid      = &xpr[kXprHeaderBytes + sysMemSize];
    const unsigned int   lowBytes = vidMemSize - split.lowOffset;

    // TXLO: the XPR2 header + sysmem block (so the texture header is complete)
    // followed by the small-mip bytes only.
    Entry lo;
    lo.hash = hash; lo.type = spak::kTypeTexLo; lo.diskGroup = kGroupLow;
    lo.sysMemSize = sysMemSize; lo.vidMemSize = vidMemSize;
    PushU32BE(lo.payload, spak::kTypeTexLo);
    PushU32BE(lo.payload, split.splitLevel);
    PushU32BE(lo.payload, split.lowOffset);
    PushU32BE(lo.payload, lowBytes);
    lo.payload.insert(lo.payload.end(), xpr.begin(),
                      xpr.begin() + kXprHeaderBytes + sysMemSize);
    lo.payload.insert(lo.payload.end(), vid + split.lowOffset, vid + vidMemSize);
    entries.push_back(lo);

    // TXHI: mip 0 plus every level coarser than the split, copied into the same
    // allocation later. Carries no headers — the TXLO already established them.
    Entry hi;
    hi.hash = spak::TexHiHash(hash); hi.type = spak::kTypeTexHi; hi.diskGroup = kGroupHigh;
    PushU32BE(hi.payload, spak::kTypeTexHi);
    PushU32BE(hi.payload, 0u);
    PushU32BE(hi.payload, split.lowOffset);
    hi.payload.insert(hi.payload.end(), vid, vid + split.lowOffset);
    entries.push_back(hi);

    printf("spakc: texture 0x%08x split at level %u (%ux%u) — %u B low / %u B total (%.1f%%)\n",
           hash, split.splitLevel, split.splitWidth, split.splitHeight,
           lowBytes, vidMemSize, 100.0 * lowBytes / (double)vidMemSize);

    seen.insert(hash);
    return hash;
}

// Read an M360 mesh, cook its textures, and add a MESH entry. meshRel is the
// scene-relative path (its nameHash is how the runtime requests the mesh).
bool AddMesh(std::vector<Entry>& entries, std::set<unsigned int>& seen,
             const std::string& rootAbs, const std::string& meshRel)
{
    const std::string meshAbs = AbsFrom(rootAbs, meshRel);
    std::vector<unsigned char> blob;
    if (!ReadFileBytes(meshAbs, blob) || blob.size() < 28)
    { fprintf(stderr, "spakc: cannot read mesh %s\n", meshAbs.c_str()); return false; }
    if (memcmp(&blob[0], "M360", 4) != 0)
    { fprintf(stderr, "spakc: bad mesh magic %s\n", meshRel.c_str()); return false; }

    const unsigned int version = ReadU32LE(&blob[4]);
    if (version != 10)
    { fprintf(stderr, "spakc: mesh %s is format v%u (need v10) - open the project in the editor to re-bake it\n",
              meshRel.c_str(), version); return false; }

    const unsigned int vcount = ReadU32LE(&blob[8]);
    const unsigned int icount = ReadU32LE(&blob[12]);
    const unsigned int flags  = ReadU32LE(&blob[16]);
    const bool skinned = (flags & 1u) != 0;
    const unsigned int jointCount = ReadU32LE(&blob[20]);
    const unsigned int skeletonFingerprint = ReadU32LE(&blob[24]);
    if ((skinned && (jointCount == 0 || jointCount > spak::kMaxSkinJoints)) ||
        (!skinned && jointCount != 0))
    { fprintf(stderr, "spakc: mesh has invalid skin metadata %s\n", meshRel.c_str()); return false; }
    const size_t vbytes = (size_t)vcount * spak::kMeshVertexBytes;
    const size_t ibytes = (size_t)icount * 4;
    if (vcount == 0 || icount == 0 || 28 + vbytes + ibytes > blob.size())
    { fprintf(stderr, "spakc: mesh empty/truncated %s\n", meshRel.c_str()); return false; }

    // Material subset table follows the buffers: u32 count, then per subset
    // u32 indexStart, u32 indexCount and 7 texture refs (u32 LE length + bytes).
    size_t off = 28 + vbytes + ibytes;
    struct SubsetIn { unsigned int start, count; std::string tex[spak::kMeshTexSlots]; };
    std::vector<SubsetIn> subs;
    {
        if (off + 4 > blob.size())
        { fprintf(stderr, "spakc: mesh missing subset table %s\n", meshRel.c_str()); return false; }
        const unsigned int subsetCount = ReadU32LE(&blob[off]); off += 4;
        if (subsetCount == 0 || subsetCount > 1024)
        { fprintf(stderr, "spakc: mesh bad subset count %u %s\n", subsetCount, meshRel.c_str()); return false; }
        for (unsigned int i = 0; i < subsetCount; ++i)
        {
            SubsetIn si; si.start = 0; si.count = 0;
            if (off + 8 > blob.size())
            { fprintf(stderr, "spakc: mesh subset truncated %s\n", meshRel.c_str()); return false; }
            si.start = ReadU32LE(&blob[off]); off += 4;
            si.count = ReadU32LE(&blob[off]); off += 4;
            for (int s = 0; s < (int)spak::kMeshTexSlots; ++s)
            {
                if (off + 4 > blob.size()) break;
                unsigned int len = ReadU32LE(&blob[off]); off += 4;
                if (len > 0 && len < 4096 && off + len <= blob.size())
                { si.tex[s].assign((const char*)&blob[off], len); off += len; }
            }
            if ((size_t)si.start + si.count > icount)
            { fprintf(stderr, "spakc: mesh subset range out of bounds %s\n", meshRel.c_str()); return false; }
            subs.push_back(si);
        }
    }

    std::vector<unsigned char> skinPayload;
    if (skinned)
    {
        const size_t influenceBytes = (size_t)vcount * spak::kSkinInfluenceBytes;
        if (off + influenceBytes > blob.size())
        { fprintf(stderr, "spakc: mesh skin stream truncated %s\n", meshRel.c_str()); return false; }
        skinPayload.insert(skinPayload.end(), blob.begin() + off, blob.begin() + off + influenceBytes);
        off += influenceBytes;

        for (unsigned int i = 0; i < jointCount; ++i)
        {
            if (off + 4 > blob.size())
            { fprintf(stderr, "spakc: mesh skeleton truncated %s\n", meshRel.c_str()); return false; }
            const unsigned int nameLen = ReadU32LE(&blob[off]); off += 4;
            if (nameLen == 0 || nameLen >= 4096 || off + nameLen + 4 + 128 > blob.size())
            { fprintf(stderr, "spakc: mesh has invalid joint %u %s\n", i, meshRel.c_str()); return false; }
            const std::string jointName((const char*)&blob[off], nameLen); off += nameLen;
            const unsigned int parent = ReadU32LE(&blob[off]); off += 4;
            if (parent != 0xFFFFFFFFu && parent >= i)
            { fprintf(stderr, "spakc: mesh has invalid joint parent %u %s\n", i, meshRel.c_str()); return false; }
            PushU32BE(skinPayload, spak::NameHash(jointName.c_str()));
            PushU32BE(skinPayload, parent);
            PushSwappedWords(skinPayload, &blob[off], 128);
            off += 128;
        }
    }

    // Blendshapes close the blob; they cook into their own 'MRPH' entry.
    std::vector<unsigned char> morphPayload;
    unsigned int morphTargets = 0;
    if ((flags & 2u) != 0)
    {
        if (off + 12 > blob.size())
        { fprintf(stderr, "spakc: mesh morph header truncated %s\n", meshRel.c_str()); return false; }
        const unsigned int targetCount = ReadU32LE(&blob[off]); off += 4;
        const unsigned int firstVertex = ReadU32LE(&blob[off]); off += 4;
        const unsigned int morphVerts  = ReadU32LE(&blob[off]); off += 4;
        if (targetCount == 0 || targetCount > spak::kMaxMorphTargets ||
            morphVerts == 0 || morphVerts > spak::kMaxMorphVertices ||
            (size_t)firstVertex + morphVerts > vcount)
        { fprintf(stderr, "spakc: mesh has invalid morph block %s\n", meshRel.c_str()); return false; }

        PushU32BE(morphPayload, spak::kMorphMagic);
        PushU32BE(morphPayload, targetCount);
        PushU32BE(morphPayload, firstVertex);
        PushU32BE(morphPayload, morphVerts);
        for (unsigned int t = 0; t < targetCount; ++t)
        {
            // The name is read only to skip it.
            if (off + 4 > blob.size())
            { fprintf(stderr, "spakc: mesh morph target truncated %s\n", meshRel.c_str()); return false; }
            const unsigned int nameLen = ReadU32LE(&blob[off]); off += 4;
            if (nameLen >= 4096 || off + nameLen + 12 > blob.size())
            { fprintf(stderr, "spakc: mesh morph target truncated %s\n", meshRel.c_str()); return false; }
            off += nameLen;
            const unsigned int shape      = ReadU32LE(&blob[off]); off += 4;
            const unsigned int scaleBits  = ReadU32LE(&blob[off]); off += 4;
            const unsigned int deltaCount = ReadU32LE(&blob[off]); off += 4;
            const size_t deltaBytes = (size_t)deltaCount * spak::kMorphDeltaBytes;
            if (deltaCount == 0 || deltaCount > morphVerts || off + deltaBytes > blob.size())
            { fprintf(stderr, "spakc: mesh morph deltas truncated %s\n", meshRel.c_str()); return false; }

            PushU32BE(morphPayload, shape);
            PushU32BE(morphPayload, scaleBits);
            PushU32BE(morphPayload, deltaCount);
            // u16 + 3*i16 + 4*i8: the 16-bit fields swap, the bytes do not, so
            // PushSwappedWords (32-bit words) cannot do it.
            for (unsigned int d = 0; d < deltaCount; ++d)
            {
                const unsigned char* record = &blob[off + (size_t)d * spak::kMorphDeltaBytes];
                for (int field = 0; field < 4; ++field)
                {
                    morphPayload.push_back(record[field * 2 + 1]);
                    morphPayload.push_back(record[field * 2 + 0]);
                }
                for (int byte = 8; byte < 12; ++byte)
                    morphPayload.push_back(record[byte]);
            }
            off += deltaBytes;
            ++morphTargets;
        }
    }

    const std::string meshDir = DirOf(meshAbs);
    // Bake all slots NON-sRGB to match the raw D3DXCreateTextureFromFile path the
    // runtime otherwise uses (which does no gamma decode on sample) — baking the
    // diffuse sRGB decodes it to linear on sample and shifts the whole scene
    // darker/more saturated. A gamma-correct pipeline is a separate future change.
    // Diffuse (slot 0) + emissive (3) + metallic (4) + clearcoat (5) +
    // roughness (6) = DXT5; normal + specular (1,2) = uncompressed A8R8G8B8:
    // DXT block-quantises a normal map's smooth gradients into a visible grid.
    // Roughness takes DXT5 for the memory: the shader reads .r, i.e. the 5-bit
    // endpoint channel, so a smooth roughness ramp quantises to ~32 steps —
    // acceptable on authored (noisy) Substance output, and a 2048-square map
    // costs 4 MB here instead of 16.
    const bool  slotSRGB[spak::kMeshTexSlots]   = { false, false, false, false, false, false, false };
    const char* slotFormat[spak::kMeshTexSlots] = { "D3DFMT_DXT5", "D3DFMT_A8R8G8B8", "D3DFMT_A8R8G8B8",
                                                    "D3DFMT_DXT5", "D3DFMT_DXT5", "D3DFMT_DXT5", "D3DFMT_DXT5" };

    // Cook each subset's textures (deduped across subsets/meshes via `seen`) and
    // classify each subset's alpha from its own diffuse.
    std::vector<unsigned int> subHash(subs.size() * spak::kMeshTexSlots, 0);
    std::vector<unsigned int> subAlpha(subs.size(), spak::kAlphaOpaque);
    unsigned int texTotal = 0;
    for (size_t i = 0; i < subs.size(); ++i)
    {
        for (int s = 0; s < (int)spak::kMeshTexSlots; ++s)
        {
            if (subs[i].tex[s].empty()) continue;
            const std::string texAbs = AbsFrom(meshDir, subs[i].tex[s]);
            std::string key = texAbs;
            for (size_t c = 0; c < key.size(); ++c) key[c] = (char)tolower((unsigned char)key[c]);
            const unsigned int h = spak::NameHash(key.c_str());
            subHash[i * spak::kMeshTexSlots + s] =
                AddTexture(entries, seen, texAbs, subs[i].tex[s], h, slotSRGB[s], slotFormat[s]);
            if (subHash[i * spak::kMeshTexSlots + s] != 0) ++texTotal;
            if (s == 0)
                subAlpha[i] = ClassifyAlpha(texAbs);
            if (s == 1 && NormalAlphaHasHeight(texAbs))
                subAlpha[i] |= spak::kAlphaHeightBit; // bump offset from the normal's alpha
        }
    }

    // MESH payload: header (BE) + subsets + native-BE GPU streams + skeleton.
    Entry e;
    e.hash = spak::NameHash(meshRel.c_str());
    e.type = spak::kTypeMesh;
    PushU32BE(e.payload, skinned ? spak::kSkinMeshMagic : spak::kMeshMagic);
    PushU32BE(e.payload, vcount);
    PushU32BE(e.payload, icount);
    PushU32BE(e.payload, (unsigned int)subs.size());
    if (skinned)
    {
        PushU32BE(e.payload, jointCount);
        PushU32BE(e.payload, skeletonFingerprint);
    }
    for (size_t i = 0; i < subs.size(); ++i)
    {
        PushU32BE(e.payload, subs[i].start);
        PushU32BE(e.payload, subs[i].count);
        PushU32BE(e.payload, subAlpha[i]);
        for (unsigned int s = 0; s < spak::kMeshTexSlots; ++s)
            PushU32BE(e.payload, subHash[i * spak::kMeshTexSlots + s]);
    }
    PushSwappedWords(e.payload, &blob[28], vbytes);
    if (skinned)
        e.payload.insert(e.payload.end(), skinPayload.begin(),
                         skinPayload.begin() + (size_t)vcount * spak::kSkinInfluenceBytes);
    PushSwappedWords(e.payload, &blob[28 + vbytes], ibytes);
    if (skinned)
        e.payload.insert(e.payload.end(),
                         skinPayload.begin() + (size_t)vcount * spak::kSkinInfluenceBytes,
                         skinPayload.end());
    const unsigned int headerBytes = skinned ? spak::kSkinMeshHeaderBytes : spak::kMeshHeaderBytes;
    e.sysMemSize = headerBytes + (unsigned int)subs.size() * spak::kMeshSubsetBytes +
                   (skinned ? jointCount * spak::kSkinJointBytes : 0);
    e.vidMemSize = (unsigned int)(vbytes + ibytes +
                   (skinned ? (size_t)vcount * spak::kSkinInfluenceBytes : 0));
    entries.push_back(e);

    if (!morphPayload.empty())
    {
        Entry m;
        m.hash = spak::NameHash((meshRel + spak::kMorphKeySuffix).c_str());
        m.type = spak::kTypeMorph;
        m.payload.swap(morphPayload);
        // All system memory: deltas and the base pose never reach the GPU.
        m.sysMemSize = (unsigned int)(m.payload.size() + vbytes);
        m.vidMemSize = 0;
        entries.push_back(m);
    }

        printf("       mesh %s  v=%u i=%u subsets=%u joints=%u tex=%u shapes=%u\n",
            meshRel.c_str(), vcount, icount, (unsigned int)subs.size(), jointCount, texTotal,
            morphTargets);
    return true;
}

// Add a video entry: the .mpg's bytes verbatim (the format is already
// compressed; storing it raw is what lets the runtime stream it with ranged
// reads instead of loading the whole clip).
bool AddVideo(std::vector<Entry>& entries, const std::string& rootAbs, const std::string& videoRel)
{
    const std::string videoAbs = AbsFrom(rootAbs, videoRel);
    Entry e;
    if (!ReadFileBytes(videoAbs, e.payload) || e.payload.empty())
    { fprintf(stderr, "spakc: cannot read video %s\n", videoAbs.c_str()); return false; }
    e.hash = spak::NameHash(videoRel.c_str());
    e.type = spak::kTypeVideo;
    e.noCompress = true;
    entries.push_back(e);
    printf("spakc: video %s — %u bytes (raw)\n", videoRel.c_str(), (unsigned int)e.payload.size());
    return true;
}

// Cook a standalone Image attribute source into the same TX2D format used by
// mesh materials. The scene-relative path is the runtime lookup key.
bool AddImage(std::vector<Entry>& entries, std::set<unsigned int>& seen,
              const std::string& rootAbs, const std::string& imageRel)
{
    const std::string imageAbs = AbsFrom(rootAbs, imageRel);
    const unsigned int hash = spak::NameHash(imageRel.c_str());
    const unsigned int alpha = ClassifyAlpha(imageAbs);
    const char* format = alpha == spak::kAlphaOpaque ? "D3DFMT_DXT1" : "D3DFMT_DXT5";
    // Image attributes and gui.* textures load whole, never low-res first.
    if (AddTexture(entries, seen, imageAbs, imageRel, hash, false, format, false) == 0)
    {
        fprintf(stderr, "spakc: cannot cook image %s\n", imageAbs.c_str());
        return false;
    }
    printf("spakc: image %s (%s)\n", imageRel.c_str(), format);
    return true;
}

// Cook an animated GIF into a stacked texture (one slice per frame) plus a
// small 'GIFA' record holding the frame count and the GIF's own per-frame
// delays. The console never decodes a GIF; playback there is just a change of
// which slice the pixel shader fetches.
//
// Shaped like AddFont: the metadata entry sits at the asset's natural path and
// names its companion pixel entry at "<path>#frames".
bool AddGif(std::vector<Entry>& entries, std::set<unsigned int>& seen,
            const std::string& rootAbs, const std::string& gifRel)
{
    const std::string gifAbs = AbsFrom(rootAbs, gifRel);
    std::vector<unsigned char> bytes;
    if (!ReadFileBytes(gifAbs, bytes) || bytes.empty())
    { fprintf(stderr, "spakc: cannot read gif %s\n", gifAbs.c_str()); return false; }

    // stb composites GIF frame disposal itself, so every frame it returns is a
    // full canvas — no inter-frame patching on our side.
    int* delays = NULL;
    int w = 0, h = 0, frameCount = 0, comp = 0;
    unsigned char* frames = stbi_load_gif_from_memory(
        &bytes[0], (int)bytes.size(), &delays, &w, &h, &frameCount, &comp, 4);
    if (!frames || frameCount <= 0 || w <= 0 || h <= 0)
    {
        if (delays) stbi_image_free(delays);
        if (frames) stbi_image_free(frames);
        fprintf(stderr, "spakc: cannot decode gif %s\n", gifAbs.c_str());
        return false;
    }
    if (frameCount > (int)spak::kMaxGifFrames)
    {
        stbi_image_free(delays);
        stbi_image_free(frames);
        fprintf(stderr, "spakc: gif %s has %d frames — the Xbox 360 array-texture "
                        "limit is %u slices, so it cannot be cooked. Shorten it or "
                        "drop its frame rate.\n",
                gifRel.c_str(), frameCount, spak::kMaxGifFrames);
        return false;
    }

    // One temp TGA per frame, written NEXT TO the RDF and referenced by bare
    // filename: Bundler caps an element's tag text at 2048 characters, which 64
    // absolute paths exceed on their own.
    const std::string tmpDir = DirOf(g_tmpBase);
    const size_t framePixels = (size_t)w * (size_t)h;
    std::vector<std::string> sourceNames;
    std::vector<std::string> sourcePaths;
    bool wroteAll = true;
    for (int f = 0; f < frameCount && wroteAll; ++f)
    {
        char name[32];
        sprintf(name, "spakc_gif%02d.tga", f);
        const std::string abs = tmpDir.empty() ? std::string(name) : tmpDir + "\\" + name;
        wroteAll = WriteRgbaTga(abs, frames + framePixels * 4 * (size_t)f, w, h);
        sourceNames.push_back(name);
        sourcePaths.push_back(abs);
    }

    const unsigned int alpha = ClassifyAlphaRgba(frames, (int)(framePixels * (size_t)frameCount));
    const char* format = alpha == spak::kAlphaOpaque ? "D3DFMT_DXT1" : "D3DFMT_DXT5";

    const std::string framesLogical = gifRel + "#frames";
    const unsigned int framesHash = spak::NameHash(framesLogical.c_str());
    const bool cooked = wroteAll &&
        AddArrayTexture(entries, seen, sourceNames, framesLogical, framesHash,
                        /*sRGB=*/false, format) != 0;
    for (size_t i = 0; i < sourcePaths.size(); ++i)
        DeleteFileA(sourcePaths[i].c_str());

    if (!cooked)
    {
        stbi_image_free(delays);
        stbi_image_free(frames);
        fprintf(stderr, "spakc: cannot cook gif frames %s\n", gifAbs.c_str());
        return false;
    }

    Entry entry;
    entry.hash = spak::NameHash(gifRel.c_str());
    entry.type = spak::kTypeGif;
    PushU32BE(entry.payload, spak::kGifMagic);
    PushU32BE(entry.payload, spak::kGifVersion);
    PushU32BE(entry.payload, framesHash);
    PushU32BE(entry.payload, (unsigned int)w);
    PushU32BE(entry.payload, (unsigned int)h);
    PushU32BE(entry.payload, (unsigned int)frameCount);
    for (int f = 0; f < frameCount; ++f)
    {
        // stb hands back centiseconds already converted to ms; 0 means "as fast
        // as possible", which every decoder clamps — gifanim::FrameDelaySeconds
        // uses 100 ms, so record it as authored and let the clock decide.
        const int ms = delays ? delays[f] : 0;
        PushU32BE(entry.payload, (unsigned int)(ms > 0 ? ms : 0));
    }
    entries.push_back(entry);

    stbi_image_free(delays);
    stbi_image_free(frames);
    printf("spakc: gif %s — %d frames of %dx%d (%s)\n",
           gifRel.c_str(), frameCount, w, h, format);
    return true;
}

// Add an audio entry: the .mp2's bytes verbatim (already compressed; raw so
// the AudioPlayer can read the entry's byte window straight out of the pak).
bool AddAudio(std::vector<Entry>& entries, const std::string& rootAbs, const std::string& audioRel)
{
    const std::string audioAbs = AbsFrom(rootAbs, audioRel);
    Entry e;
    if (!ReadFileBytes(audioAbs, e.payload) || e.payload.empty())
    { fprintf(stderr, "spakc: cannot read audio %s\n", audioAbs.c_str()); return false; }
    e.hash = spak::NameHash(audioRel.c_str());
    e.type = spak::kTypeAudio;
    e.noCompress = true;
    entries.push_back(e);
    printf("spakc: audio %s — %u bytes (raw)\n", audioRel.c_str(), (unsigned int)e.payload.size());
    return true;
}

struct CookedGlyph
{
    unsigned int codepoint;
    float advance, bearingX, bearingY, width, height;
    float u0, v0, u1, v1;
};

struct CookedKerning
{
    unsigned int left, right;
    float advance;
};

bool AddFont(std::vector<Entry>& entries, std::set<unsigned int>& seen,
             const std::string& rootAbs, const std::string& fontRel)
{
    const int atlasWidth = 1024;
    const int atlasHeight = 1024;
    const float sourcePixelSize = 48.0f;
    const int sdfPadding = 5;
    const unsigned char sdfOnEdge = 128;
    const float sdfPixelDistScale = 128.0f / (float)sdfPadding;
    const std::string fontAbs = AbsFrom(rootAbs, fontRel);
    std::vector<unsigned char> fontBytes;
    if (!ReadFileBytes(fontAbs, fontBytes) || fontBytes.empty())
    { fprintf(stderr, "spakc: cannot read font %s\n", fontAbs.c_str()); return false; }

    const int fontOffset = stbtt_GetFontOffsetForIndex(&fontBytes[0], 0);
    stbtt_fontinfo fontInfo;
    if (fontOffset < 0 || !stbtt_InitFont(&fontInfo, &fontBytes[0], fontOffset))
    { fprintf(stderr, "spakc: invalid TTF/OTF font %s\n", fontAbs.c_str()); return false; }
    if (stbtt_FindGlyphIndex(&fontInfo, '?') == 0)
    { fprintf(stderr, "spakc: font has no '?' fallback glyph %s\n", fontAbs.c_str()); return false; }

    const float scale = stbtt_ScaleForPixelHeight(&fontInfo, sourcePixelSize);
    std::vector<unsigned char> atlas((size_t)atlasWidth * atlasHeight, 0);
    std::vector<CookedGlyph> glyphs;
    int nextX = 1, nextY = 1, rowHeight = 0;
    for (unsigned int codepoint = 0x20; codepoint <= 0xFF; ++codepoint)
    {
        if (codepoint > 0x7E && codepoint < 0xA0)
            continue;
        if (stbtt_FindGlyphIndex(&fontInfo, (int)codepoint) == 0)
            continue;

        int advance = 0, leftBearing = 0;
        stbtt_GetCodepointHMetrics(&fontInfo, (int)codepoint, &advance, &leftBearing);
        CookedGlyph glyph;
        glyph.codepoint = codepoint;
        glyph.advance = advance * scale;
        glyph.bearingX = 0.0f;
        glyph.bearingY = 0.0f;
        glyph.width = glyph.height = 0.0f;
        glyph.u0 = glyph.v0 = glyph.u1 = glyph.v1 = 0.0f;

        int bitmapWidth = 0, bitmapHeight = 0, xOffset = 0, yOffset = 0;
        unsigned char* bitmap = stbtt_GetCodepointSDF(
            &fontInfo, scale, (int)codepoint, sdfPadding, sdfOnEdge,
            sdfPixelDistScale, &bitmapWidth, &bitmapHeight, &xOffset, &yOffset);
        if (bitmap && bitmapWidth > 0 && bitmapHeight > 0)
        {
            if (nextX + bitmapWidth + 1 > atlasWidth)
            { nextX = 1; nextY += rowHeight + 1; rowHeight = 0; }
            if (nextY + bitmapHeight + 1 > atlasHeight)
            {
                stbtt_FreeSDF(bitmap, fontInfo.userdata);
                fprintf(stderr, "spakc: font atlas overflow %s\n", fontAbs.c_str());
                return false;
            }
            for (int row = 0; row < bitmapHeight; ++row)
                memcpy(&atlas[(size_t)(nextY + row) * atlasWidth + nextX],
                       bitmap + (size_t)row * bitmapWidth, bitmapWidth);
            glyph.bearingX = (float)xOffset;
            glyph.bearingY = (float)-yOffset;
            glyph.width = (float)bitmapWidth;
            glyph.height = (float)bitmapHeight;
            glyph.u0 = (float)nextX / atlasWidth;
            glyph.v0 = (float)nextY / atlasHeight;
            glyph.u1 = (float)(nextX + bitmapWidth) / atlasWidth;
            glyph.v1 = (float)(nextY + bitmapHeight) / atlasHeight;
            nextX += bitmapWidth + 1;
            if (bitmapHeight > rowHeight) rowHeight = bitmapHeight;
        }
        if (bitmap) stbtt_FreeSDF(bitmap, fontInfo.userdata);
        glyphs.push_back(glyph);
    }
    if (glyphs.empty() || glyphs.size() > spak::kMaxFontGlyphs)
    { fprintf(stderr, "spakc: invalid cooked glyph count %s\n", fontAbs.c_str()); return false; }

    std::vector<CookedKerning> kerning;
    for (size_t left = 0; left < glyphs.size(); ++left)
        for (size_t right = 0; right < glyphs.size(); ++right)
        {
            const int amount = stbtt_GetCodepointKernAdvance(
                &fontInfo, (int)glyphs[left].codepoint, (int)glyphs[right].codepoint);
            if (amount != 0)
            {
                CookedKerning pair;
                pair.left = glyphs[left].codepoint;
                pair.right = glyphs[right].codepoint;
                pair.advance = amount * scale;
                kerning.push_back(pair);
            }
        }
    if (kerning.size() > spak::kMaxFontKerning)
    { fprintf(stderr, "spakc: excessive kerning pairs %s\n", fontAbs.c_str()); return false; }

    const std::string atlasLogical = fontRel + "#atlas";
    const unsigned int atlasHash = spak::NameHash(atlasLogical.c_str());
    const std::string atlasTga = g_tmpBase + ".font.tmp.tga";
    // Never split an SDF atlas: a 32px mip of signed-distance data is meaningless,
    // and text already declines to draw until its atlas is fully resident.
    if (!WriteAtlasTga(atlasTga, atlas, atlasWidth, atlasHeight) ||
        AddTexture(entries, seen, atlasTga, atlasLogical, atlasHash, false, "D3DFMT_A8",
                   /*allowSplit=*/false) == 0)
    {
        DeleteFileA(atlasTga.c_str());
        fprintf(stderr, "spakc: cannot cook font atlas %s\n", fontAbs.c_str());
        return false;
    }
    DeleteFileA(atlasTga.c_str());

    int ascent = 0, descent = 0, lineGap = 0;
    stbtt_GetFontVMetrics(&fontInfo, &ascent, &descent, &lineGap);
    Entry entry;
    entry.hash = spak::NameHash(fontRel.c_str());
    entry.type = spak::kTypeFont;
    PushU32BE(entry.payload, spak::kFontMagic);
    PushU32BE(entry.payload, spak::kFontVersion);
    PushU32BE(entry.payload, atlasHash);
    PushU32BE(entry.payload, atlasWidth);
    PushU32BE(entry.payload, atlasHeight);
    PushF32BE(entry.payload, sourcePixelSize);
    PushF32BE(entry.payload, ascent * scale);
    PushF32BE(entry.payload, descent * scale);
    PushF32BE(entry.payload, (ascent - descent + lineGap) * scale);
    PushF32BE(entry.payload, (float)sdfPadding);
    PushU32BE(entry.payload, (unsigned int)glyphs.size());
    PushU32BE(entry.payload, (unsigned int)kerning.size());
    for (size_t i = 0; i < glyphs.size(); ++i)
    {
        PushU32BE(entry.payload, glyphs[i].codepoint);
        PushF32BE(entry.payload, glyphs[i].advance);
        PushF32BE(entry.payload, glyphs[i].bearingX);
        PushF32BE(entry.payload, glyphs[i].bearingY);
        PushF32BE(entry.payload, glyphs[i].width);
        PushF32BE(entry.payload, glyphs[i].height);
        PushF32BE(entry.payload, glyphs[i].u0); PushF32BE(entry.payload, glyphs[i].v0);
        PushF32BE(entry.payload, glyphs[i].u1); PushF32BE(entry.payload, glyphs[i].v1);
    }
    for (size_t i = 0; i < kerning.size(); ++i)
    {
        PushU32BE(entry.payload, kerning[i].left);
        PushU32BE(entry.payload, kerning[i].right);
        PushF32BE(entry.payload, kerning[i].advance);
    }
    entry.sysMemSize = (unsigned int)entry.payload.size();
    entries.push_back(entry);
    printf("spakc: font %s — glyphs=%u kern=%u atlas=%dx%d\n", fontRel.c_str(),
           (unsigned int)glyphs.size(), (unsigned int)kerning.size(), atlasWidth, atlasHeight);
    return true;
}

bool AddAnimation(std::vector<Entry>& entries, const std::string& source,
                  const std::string& logicalName)
{
    Entry e;
    if (!ReadFileBytes(source, e.payload) || e.payload.size() < 8)
    { fprintf(stderr, "spakc: cannot read animation %s\n", source.c_str()); return false; }
    const unsigned int magic = ReadU32BE(&e.payload[0]);
    if (magic != 0x414E4D31u && magic != 0x414E4D32u && magic != 0x414E4331u) // ANM1/ANM2 clip or ANC1 controller
    { fprintf(stderr, "spakc: bad animation magic %s\n", source.c_str()); return false; }
    e.hash = spak::NameHash(logicalName.c_str());
    e.type = spak::kTypeAnim;
    e.noCompress = true;
    e.sysMemSize = (unsigned int)e.payload.size();
    entries.push_back(e);
    printf("spakc: animation %s <- %s — %u bytes (raw)\n", logicalName.c_str(),
           source.c_str(), (unsigned int)e.payload.size());
    return true;
}

// Add a cooked face clip (facec output). Raw like an animation: the runtime
// reads it with a ranged read and samples the frames where they lie.
bool AddFaceClip(std::vector<Entry>& entries, const std::string& source,
                 const std::string& logicalName)
{
    Entry e;
    if (!ReadFileBytes(source, e.payload) || e.payload.size() < 8)
    { fprintf(stderr, "spakc: cannot read face clip %s\n", source.c_str()); return false; }
    if (ReadU32BE(&e.payload[0]) != face::kClipMagic)
    { fprintf(stderr, "spakc: bad face clip magic %s\n", source.c_str()); return false; }
    e.hash = spak::NameHash(logicalName.c_str());
    e.type = spak::kTypeFace;
    e.noCompress = true;
    e.sysMemSize = (unsigned int)e.payload.size();
    entries.push_back(e);
    printf("spakc: face clip %s <- %s — %u bytes (raw)\n", logicalName.c_str(),
           source.c_str(), (unsigned int)e.payload.size());
    return true;
}

bool WriteSpak(const std::string& outPath, std::vector<Entry>& entries, bool compress)
{
    if (entries.empty()) { fprintf(stderr, "spakc: nothing to write\n"); return false; }
    // Compress payloads; entries sorted by hash for binary search.
    std::sort(entries.begin(), entries.end(), EntryLess);
    const unsigned int count  = (unsigned int)entries.size();
    const unsigned int sector = spak::kSectorSize;

    // Find() binary-searches the TOC, so two entries sharing a key would make one
    // of them unreachable. Cheap to check, and it is the one way a derived TXHI
    // key could go wrong.
    for (unsigned int i = 1; i < count; ++i)
        if (entries[i].hash == entries[i - 1].hash)
        {
            fprintf(stderr, "spakc: DUPLICATE entry key 0x%08x (types 0x%08x / 0x%08x)\n",
                    entries[i].hash, entries[i - 1].type, entries[i].type);
                printf("spakc: DUPLICATE entry key 0x%08x (types 0x%08x / 0x%08x)\n",
                   entries[i].hash, entries[i - 1].type, entries[i].type);
            return false;
        }

    std::vector<std::vector<unsigned char> > diskPayload(count);
    std::vector<unsigned int> compSize(count), uncompSize(count), codec(count);
    for (unsigned int i = 0; i < count; ++i)
    {
        uncompSize[i] = (unsigned int)entries[i].payload.size();
        if (compress && !entries[i].noCompress)
        {
            if (LzxCompress(&entries[i].payload[0], entries[i].payload.size(), diskPayload[i]) == 0)
            {
                printf("spakc: compression failed for entry 0x%08x type 0x%08x bytes=%u\n",
                       entries[i].hash, entries[i].type, uncompSize[i]);
                return false;
            }
            if (!LzxVerify(diskPayload[i], uncompSize[i]))
            {
                fprintf(stderr, "spakc: SELF-VERIFY FAILED for entry 0x%08x\n", entries[i].hash);
                printf("spakc: SELF-VERIFY FAILED for entry 0x%08x type 0x%08x\n",
                       entries[i].hash, entries[i].type);
                return false;
            }
            codec[i] = spak::kCodecLZX;
        }
        else
        {
            diskPayload[i] = entries[i].payload;
            codec[i] = spak::kCodecNone;
        }
        compSize[i] = (unsigned int)diskPayload[i].size();
        std::vector<unsigned char>().swap(entries[i].payload);
    }

    // Assign sector-aligned disk offsets in DISK order, which is by group (all the
    // small-mip chunks first, full-resolution data last) and not the TOC's hash
    // order. The TOC carries an explicit diskOffset, so the two are free to differ.
    std::vector<unsigned int> diskOrder(count);
    for (unsigned int i = 0; i < count; ++i) diskOrder[i] = i;
    for (unsigned int i = 1; i < count; ++i) // insertion sort: stable, C++03, tiny n
    {
        const unsigned int v = diskOrder[i];
        unsigned int j = i;
        while (j > 0 && entries[diskOrder[j - 1]].diskGroup > entries[v].diskGroup)
        { diskOrder[j] = diskOrder[j - 1]; --j; }
        diskOrder[j] = v;
    }

    const unsigned int tocOffset = spak::kHeaderBytes;
    unsigned int cursor = tocOffset + count * spak::kEntryBytes;
    cursor = ((cursor + sector - 1) / sector) * sector;
    std::vector<unsigned int> diskOffset(count);
    for (unsigned int k = 0; k < count; ++k)
    {
        const unsigned int i = diskOrder[k];
        diskOffset[i] = cursor;
        cursor += ((compSize[i] + sector - 1) / sector) * sector;
    }

    // Build the file.
    std::vector<unsigned char> file;
    PushU32BE(file, spak::kMagic);
    PushU32BE(file, spak::kVersion);
    PushU32BE(file, count);
    PushU32BE(file, sector);
    PushU32BE(file, tocOffset);
    for (unsigned int i = 0; i < count; ++i)
    {
        PushU32BE(file, entries[i].hash);
        PushU32BE(file, entries[i].type);
        PushU32BE(file, spak::MakeFlags(codec[i]));
        PushU32BE(file, diskOffset[i]);
        PushU32BE(file, compSize[i]);
        PushU32BE(file, uncompSize[i]);
        PushU32BE(file, entries[i].sysMemSize);
        PushU32BE(file, entries[i].vidMemSize);
    }
    for (unsigned int k = 0; k < count; ++k) // payloads follow disk order, not TOC order
    {
        const unsigned int i = diskOrder[k];
        file.resize(diskOffset[i], 0);
        file.insert(file.end(), diskPayload[i].begin(), diskPayload[i].end());
    }
    file.resize(((file.size() + sector - 1) / sector) * sector, 0);

    if (!WriteFileBytes(outPath, &file[0], file.size()))
    {
        fprintf(stderr, "spakc: cannot write %s\n", outPath.c_str());
        printf("spakc: cannot write output %s bytes=%u\n", outPath.c_str(), (unsigned int)file.size());
        return false;
    }

    unsigned int totalUncomp = 0, totalComp = 0;
    for (unsigned int i = 0; i < count; ++i) { totalUncomp += uncompSize[i]; totalComp += compSize[i]; }
    printf("spakc: wrote %s — %u entries, %u -> %s %u bytes, file %u bytes  [verify OK]\n",
           outPath.c_str(), count, totalUncomp, compress ? "LZX" : "raw", totalComp,
           (unsigned int)file.size());
    return true;
}

bool ResolveBundler()
{
    const char* xedk = getenv("XEDK");
    if (!xedk || !*xedk) { fprintf(stderr, "spakc: XEDK not set (needed for Bundler.exe)\n"); return false; }
    g_bundler = std::string(xedk) + "\\bin\\win32\\Bundler.exe";
    return true;
}

bool AddLightmapSidecar(std::vector<Entry>& entries, const std::string& rootAbs,
                        const std::string& rel, unsigned int type)
{
    Entry entry;
    entry.hash = spak::NameHash(rel.c_str());
    entry.type = type;
    entry.noCompress = true;
    if (!ReadFileBytes(AbsFrom(rootAbs, rel), entry.payload) || entry.payload.empty())
    {
        fprintf(stderr, "spakc: cannot read lightmap sidecar %s\n", rel.c_str());
        return false;
    }
    entry.sysMemSize = (unsigned int)entry.payload.size();
    if (type == spak::kTypeLmap)
    {
        if (entry.payload.size() < 40 || memcmp(&entry.payload[0], "LMP0", 4) != 0 ||
            ReadU32LE(&entry.payload[4]) != 3)
        { fprintf(stderr, "spakc: invalid LMP0 v3 metadata %s\n", rel.c_str()); return false; }
        const unsigned int instanceCount = ReadU32LE(&entry.payload[16]);
        const unsigned int probeCount = ReadU32LE(&entry.payload[20]);
        const size_t probeOffset = 40 + (size_t)instanceCount * 20;
        if (entry.payload.size() < probeOffset + (size_t)probeCount * 60)
        { fprintf(stderr, "spakc: truncated probe grid %s\n", rel.c_str()); return false; }

        unsigned int dimensions[3] = {0, 0, 0};
        float spacing = 0.0f, origin[3] = {0, 0, 0};
        memcpy(&spacing, &entry.payload[24], 4);
        memcpy(origin, &entry.payload[28], 12);
        if (probeCount && spacing <= 0.0f)
        { fprintf(stderr, "spakc: invalid probe spacing %s\n", rel.c_str()); return false; }
        for (unsigned int probe = 0; probe < probeCount; ++probe)
        {
            float position[3];
            memcpy(position, &entry.payload[probeOffset + (size_t)probe * 60], 12);
            for (int axis = 0; axis < 3; ++axis)
            {
                const unsigned int dimension = (unsigned int)floorf((position[axis] - origin[axis]) / spacing + 0.5f) + 1;
                if (dimension > dimensions[axis]) dimensions[axis] = dimension;
            }
        }
        if (probeCount && (size_t)dimensions[0] * dimensions[1] * dimensions[2] != probeCount)
        { fprintf(stderr, "spakc: irregular probe grid %s\n", rel.c_str()); return false; }

        Entry probes;
        std::string probeRel = rel;
        const size_t dot = probeRel.find_last_of('.');
        if (dot != std::string::npos) probeRel.erase(dot);
        probeRel += ".lprb";
        probes.hash = spak::NameHash(probeRel.c_str());
        probes.type = spak::kTypeLprb;
        probes.noCompress = true;
        probes.payload.insert(probes.payload.end(), "PRB0", "PRB0" + 4);
        PushU32LE(probes.payload, 1);
        probes.payload.insert(probes.payload.end(), &entry.payload[24], &entry.payload[40]);
        for (int axis = 0; axis < 3; ++axis) PushU32LE(probes.payload, dimensions[axis]);
        PushU32LE(probes.payload, probeCount);
        for (unsigned int probe = 0; probe < probeCount; ++probe)
            probes.payload.insert(probes.payload.end(),
                                  &entry.payload[probeOffset + (size_t)probe * 60 + 12],
                                  &entry.payload[probeOffset + (size_t)(probe + 1) * 60]);
        probes.sysMemSize = (unsigned int)probes.payload.size();
        entries.push_back(probes);
        printf("       probes %s  count=%u bytes=%u\n", probeRel.c_str(), probeCount,
               (unsigned int)probes.payload.size());
    }
    entries.push_back(entry);
    printf("       lightmap %s  bytes=%u\n", rel.c_str(), (unsigned int)entry.payload.size());
    return true;
}
} // namespace

int main(int argc, char** argv)
{
    if (argc >= 2 && std::string(argv[1]) == "build")
    {
        if (argc < 5)
        { fprintf(stderr, "usage: spakc build <out.spak> <contentRoot> <meshRel...> [--image <imageRel>] [--lmap <rel>] [--lmuv <rel>] [--raw]\n"); return 2; }
        const std::string outSpak = argv[2];
        const std::string root    = argv[3];
        bool compress = true;
        std::vector<std::string> meshes;
        std::vector<std::string> images;
        std::vector<std::string> videos; // .mpg args become raw VIDE entries
        std::vector<std::string> audios; // .mp2 args become raw AUDI entries
        std::vector<std::string> fonts;
        std::vector<std::string> lmaps;
        std::vector<std::string> lmuvs;
        struct AnimationArg { std::string source, logical; };
        std::vector<AnimationArg> animations;
        std::vector<AnimationArg> faceClips;
        for (int i = 4; i < argc; ++i)
        {
            const std::string arg = argv[i];
            if (arg == "--raw") { compress = false; continue; }
            if (arg == "--anim")
            {
                if (i + 2 >= argc)
                { fprintf(stderr, "spakc: --anim requires <source> <logical-name>\n"); return 2; }
                AnimationArg animation;
                animation.source = argv[++i];
                animation.logical = argv[++i];
                animations.push_back(animation);
                continue;
            }
            if (arg == "--face")
            {
                if (i + 2 >= argc)
                { fprintf(stderr, "spakc: --face requires <source> <logical-name>\n"); return 2; }
                AnimationArg clip;
                clip.source = argv[++i];
                clip.logical = argv[++i];
                faceClips.push_back(clip);
                continue;
            }
            if (arg == "--image")
            {
                if (i + 1 >= argc)
                { fprintf(stderr, "spakc: --image requires <logical-path>\n"); return 2; }
                images.push_back(argv[++i]);
                continue;
            }
            if (arg == "--font")
            {
                if (i + 1 >= argc)
                { fprintf(stderr, "spakc: --font requires <logical-path>\n"); return 2; }
                fonts.push_back(argv[++i]);
                continue;
            }
            if (arg == "--lmap" || arg == "--lmuv")
            {
                if (i + 1 >= argc)
                { fprintf(stderr, "spakc: %s requires <logical-path>\n", arg.c_str()); return 2; }
                if (arg == "--lmap") lmaps.push_back(argv[++i]);
                else                 lmuvs.push_back(argv[++i]);
                continue;
            }
            std::string ext = arg.size() >= 4 ? arg.substr(arg.size() - 4) : "";
            for (size_t c = 0; c < ext.size(); ++c)
                if (ext[c] >= 'A' && ext[c] <= 'Z') ext[c] = (char)(ext[c] - 'A' + 'a');
            if      (ext == ".mpg") videos.push_back(arg);
            else if (ext == ".mp2") audios.push_back(arg);
            else                    meshes.push_back(arg);
        }
        if (meshes.empty() && images.empty() && videos.empty() && audios.empty() && fonts.empty() && animations.empty() && faceClips.empty() && lmaps.empty() && lmuvs.empty())
        { fprintf(stderr, "spakc: no assets given\n"); return 2; }
        if (!ResolveBundler()) return 1;
        g_tmpBase = outSpak;

        const std::string rootAbs = AbsPath(root);
        std::vector<Entry> entries;
        std::set<unsigned int> seenTex;
        int okMeshes = 0, okImages = 0, okVideos = 0, okAudios = 0, okFonts = 0, okAnimations = 0, okFaceClips = 0;
        for (size_t i = 0; i < meshes.size(); ++i)
            if (AddMesh(entries, seenTex, rootAbs, meshes[i])) ++okMeshes;
        // deploy.ps1 sends every image path through --image, so route by
        // extension here rather than adding a --gif the caller would have to
        // classify for.
        for (size_t i = 0; i < images.size(); ++i)
            if (gifanim::IsGifPath(images[i]) ? AddGif(entries, seenTex, rootAbs, images[i])
                                              : AddImage(entries, seenTex, rootAbs, images[i]))
                ++okImages;
        for (size_t i = 0; i < videos.size(); ++i)
            if (AddVideo(entries, rootAbs, videos[i])) ++okVideos;
        for (size_t i = 0; i < audios.size(); ++i)
            if (AddAudio(entries, rootAbs, audios[i])) ++okAudios;
        for (size_t i = 0; i < fonts.size(); ++i)
            if (AddFont(entries, seenTex, rootAbs, fonts[i])) ++okFonts;
        for (size_t i = 0; i < animations.size(); ++i)
            if (AddAnimation(entries, animations[i].source, animations[i].logical)) ++okAnimations;
        for (size_t i = 0; i < faceClips.size(); ++i)
            if (AddFaceClip(entries, faceClips[i].source, faceClips[i].logical)) ++okFaceClips;
        for (size_t i = 0; i < lmaps.size(); ++i)
            AddLightmapSidecar(entries, rootAbs, lmaps[i], spak::kTypeLmap);
        for (size_t i = 0; i < lmuvs.size(); ++i)
            AddLightmapSidecar(entries, rootAbs, lmuvs[i], spak::kTypeLmuv);
        if (okMeshes == 0 && okImages == 0 && okVideos == 0 && okAudios == 0 && okFonts == 0 &&
            okAnimations == 0 && okFaceClips == 0)
        { fprintf(stderr, "spakc: nothing cooked\n"); return 1; }
        return WriteSpak(outSpak, entries, compress) ? 0 : 1;
    }

    // Single-texture mode: `spakc tex <src> <name> <out>` or legacy `<src> <name> <out>`.
    int base = (argc >= 2 && std::string(argv[1]) == "tex") ? 2 : 1;
    if (argc < base + 3)
    { fprintf(stderr, "usage: spakc tex <source-image> <logical-name> <out.spak> [--raw]\n"); return 2; }
    const std::string source = argv[base + 0];
    const std::string name   = argv[base + 1];
    const std::string outSpak = argv[base + 2];
    bool compress = true;
    for (int i = base + 3; i < argc; ++i) if (std::string(argv[i]) == "--raw") compress = false;
    if (!ResolveBundler()) return 1;
    g_tmpBase = outSpak;

    std::vector<unsigned char> xpr;
    if (!CookTextureXPR(AbsPath(source), name, false, "D3DFMT_DXT5", xpr))
    { fprintf(stderr, "spakc: texture cook failed\n"); return 1; }
    std::vector<Entry> entries(1);
    entries[0].hash = spak::NameHash(name.c_str());
    entries[0].type = spak::kTypeTex2D;
    entries[0].payload = xpr;
    entries[0].sysMemSize = ReadU32BE(&xpr[4]);
    entries[0].vidMemSize = ReadU32BE(&xpr[8]);
    return WriteSpak(outSpak, entries, compress) ? 0 : 1;
}
