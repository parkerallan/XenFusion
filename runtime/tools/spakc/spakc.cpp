//--------------------------------------------------------------------------------------
// spakc — Xbox 360 asset stream cooker.
//
// Modes:
//   spakc build <out.spak> <contentRoot> <meshRel1> [meshRel2 ...] [--raw]
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
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string>
#include <vector>
#include <set>
#include <algorithm>

#include "SpakFormat.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

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

// Bake an image to an XPR2 blob via Bundler. `format` is a D3DFMT_* name (DXT5
// for diffuse; A8R8G8B8 uncompressed for normal maps, whose smooth gradients DXT
// facets into visible block artifacts). Returns false on failure.
bool CookTextureXPR(const std::string& absSource, const std::string& name, bool sRGB,
                    const char* format, std::vector<unsigned char>& xprOut)
{
    const std::string rdfPath = g_tmpBase + ".tmp.rdf";
    const std::string xprPath = g_tmpBase + ".tmp.xpr";
    std::string rdf =
        "<?xml version=\"1.0\" encoding=\"utf-8\" ?>\n<RDF Version=\"XPR2\">\n"
        "  <Texture Name=\"" + name + "\" Source=\"" + absSource +
        "\" Format=\"" + format + "\" Levels=\"0\" sRGB=\"" + (sRGB ? "true" : "false") + "\" />\n</RDF>\n";
    if (!WriteFileBytes(rdfPath, (const unsigned char*)rdf.data(), rdf.size())) return false;
    bool ok = RunBundler(rdfPath, xprPath) && ReadFileBytes(xprPath, xprOut) && xprOut.size() >= 12
              && ReadU32BE(&xprOut[0]) == 0x58505232u;
    DeleteFileA(rdfPath.c_str());
    DeleteFileA(xprPath.c_str());
    return ok;
}

// Classify the diffuse alpha like the editor: no alpha -> Opaque; >~3% fully
// transparent texels (holes) -> Cutout; otherwise translucent -> Blend.
unsigned int ClassifyAlpha(const std::string& absPng)
{
    int w = 0, h = 0, ch = 0;
    unsigned char* px = stbi_load(absPng.c_str(), &w, &h, &ch, 4);
    if (!px) return spak::kAlphaOpaque;
    const int total = w * h;
    int translucent = 0, transparent = 0;
    for (int i = 0; i < total; ++i)
    {
        unsigned char a = px[i * 4 + 3];
        if (a < 255) ++translucent;
        if (a < 24)  ++transparent;
    }
    stbi_image_free(px);
    if (translucent == 0)              return spak::kAlphaOpaque;
    if (transparent > total / 32)      return spak::kAlphaCutout;
    return spak::kAlphaBlend;
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
struct Entry
{
    unsigned int hash;
    unsigned int type;
    std::vector<unsigned char> payload; // uncompressed
    unsigned int sysMemSize;
    unsigned int vidMemSize;
};

bool EntryLess(const Entry& a, const Entry& b) { return a.hash < b.hash; }

// Cook + add one texture entry, deduped by hash. `absSource` feeds Bundler;
// `hash` is the internal link key (mesh -> texture). Returns the hash (0 if the
// slot is empty or cook failed, so the runtime falls back to a default).
unsigned int AddTexture(std::vector<Entry>& entries, std::set<unsigned int>& seen,
                        const std::string& absSource, const std::string& name,
                        unsigned int hash, bool sRGB, const char* format)
{
    if (absSource.empty() || hash == 0) return 0;
    if (seen.find(hash) != seen.end()) return hash; // already cooked
    std::vector<unsigned char> xpr;
    if (!CookTextureXPR(absSource, name, sRGB, format, xpr))
    { fprintf(stderr, "spakc: WARN texture cook failed: %s\n", absSource.c_str()); return 0; }
    Entry e;
    e.hash = hash; e.type = spak::kTypeTex2D; e.payload = xpr;
    e.sysMemSize = ReadU32BE(&xpr[4]); e.vidMemSize = ReadU32BE(&xpr[8]);
    entries.push_back(e);
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
    if (!ReadFileBytes(meshAbs, blob) || blob.size() < 16)
    { fprintf(stderr, "spakc: cannot read mesh %s\n", meshAbs.c_str()); return false; }
    if (memcmp(&blob[0], "M360", 4) != 0)
    { fprintf(stderr, "spakc: bad mesh magic %s\n", meshRel.c_str()); return false; }

    const unsigned int version = ReadU32LE(&blob[4]);
    if (version != 5)
    { fprintf(stderr, "spakc: mesh %s is format v%u (need v5) - open the project in the editor to re-bake it\n",
              meshRel.c_str(), version); return false; }

    const unsigned int vcount = ReadU32LE(&blob[8]);
    const unsigned int icount = ReadU32LE(&blob[12]);
    const size_t vbytes = (size_t)vcount * spak::kMeshVertexBytes;
    const size_t ibytes = (size_t)icount * 4;
    if (vcount == 0 || icount == 0 || 16 + vbytes + ibytes > blob.size())
    { fprintf(stderr, "spakc: mesh empty/truncated %s\n", meshRel.c_str()); return false; }

    // Material subset table follows the buffers: u32 count, then per subset
    // u32 indexStart, u32 indexCount and 4 texture refs (u32 LE length + bytes).
    size_t off = 16 + vbytes + ibytes;
    struct SubsetIn { unsigned int start, count; std::string tex[4]; };
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
            for (int s = 0; s < 4; ++s)
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

    const std::string meshDir = DirOf(meshAbs);
    // Bake all slots NON-sRGB to match the raw D3DXCreateTextureFromFile path the
    // runtime otherwise uses (which does no gamma decode on sample) — baking the
    // diffuse sRGB decodes it to linear on sample and shifts the whole scene
    // darker/more saturated. A gamma-correct pipeline is a separate future change.
    // Diffuse (slot 0) + emissive (3) = DXT5; normal + specular (1,2) =
    // uncompressed A8R8G8B8: DXT block-quantises a normal map's smooth
    // gradients into a visible grid.
    const bool  slotSRGB[4]   = { false, false, false, false };
    const char* slotFormat[4] = { "D3DFMT_DXT5", "D3DFMT_A8R8G8B8", "D3DFMT_A8R8G8B8", "D3DFMT_DXT5" };

    // Cook each subset's textures (deduped across subsets/meshes via `seen`) and
    // classify each subset's alpha from its own diffuse.
    std::vector<unsigned int> subHash(subs.size() * 4, 0);
    std::vector<unsigned int> subAlpha(subs.size(), spak::kAlphaOpaque);
    unsigned int texTotal = 0;
    for (size_t i = 0; i < subs.size(); ++i)
    {
        for (int s = 0; s < 4; ++s)
        {
            if (subs[i].tex[s].empty()) continue;
            const std::string texAbs = AbsFrom(meshDir, subs[i].tex[s]);
            std::string key = texAbs;
            for (size_t c = 0; c < key.size(); ++c) key[c] = (char)tolower((unsigned char)key[c]);
            const unsigned int h = spak::NameHash(key.c_str());
            subHash[i * 4 + s] = AddTexture(entries, seen, texAbs, subs[i].tex[s], h, slotSRGB[s], slotFormat[s]);
            if (subHash[i * 4 + s] != 0) ++texTotal;
            if (s == 0)
                subAlpha[i] = ClassifyAlpha(texAbs);
            if (s == 1 && NormalAlphaHasHeight(texAbs))
                subAlpha[i] |= spak::kAlphaHeightBit; // bump offset from the normal's alpha
        }
    }

    // MESH payload: header (BE) + per-subset records + native-BE VB + native-BE IB.
    Entry e;
    e.hash = spak::NameHash(meshRel.c_str());
    e.type = spak::kTypeMesh;
    PushU32BE(e.payload, spak::kMeshMagic);
    PushU32BE(e.payload, vcount);
    PushU32BE(e.payload, icount);
    PushU32BE(e.payload, (unsigned int)subs.size());
    for (size_t i = 0; i < subs.size(); ++i)
    {
        PushU32BE(e.payload, subs[i].start);
        PushU32BE(e.payload, subs[i].count);
        PushU32BE(e.payload, subAlpha[i]);
        PushU32BE(e.payload, subHash[i * 4 + 0]);
        PushU32BE(e.payload, subHash[i * 4 + 1]);
        PushU32BE(e.payload, subHash[i * 4 + 2]);
        PushU32BE(e.payload, subHash[i * 4 + 3]);
    }
    PushSwappedWords(e.payload, &blob[16], vbytes);
    PushSwappedWords(e.payload, &blob[16 + vbytes], ibytes);
    e.sysMemSize = spak::kMeshHeaderBytes + (unsigned int)subs.size() * spak::kMeshSubsetBytes;
    e.vidMemSize = (unsigned int)(vbytes + ibytes);
    entries.push_back(e);

    printf("       mesh %s  v=%u i=%u subsets=%u tex=%u\n",
           meshRel.c_str(), vcount, icount, (unsigned int)subs.size(), texTotal);
    return true;
}

bool WriteSpak(const std::string& outPath, std::vector<Entry>& entries, bool compress)
{
    if (entries.empty()) { fprintf(stderr, "spakc: nothing to write\n"); return false; }

    // Compress payloads; entries sorted by hash for binary search.
    std::sort(entries.begin(), entries.end(), EntryLess);
    const unsigned int count  = (unsigned int)entries.size();
    const unsigned int sector = spak::kSectorSize;

    std::vector<std::vector<unsigned char> > diskPayload(count);
    std::vector<unsigned int> compSize(count), uncompSize(count), codec(count);
    for (unsigned int i = 0; i < count; ++i)
    {
        uncompSize[i] = (unsigned int)entries[i].payload.size();
        if (compress)
        {
            if (LzxCompress(&entries[i].payload[0], entries[i].payload.size(), diskPayload[i]) == 0)
                return false;
            if (!LzxVerify(diskPayload[i], uncompSize[i]))
            { fprintf(stderr, "spakc: SELF-VERIFY FAILED for entry 0x%08x\n", entries[i].hash); return false; }
            codec[i] = spak::kCodecLZX;
        }
        else
        {
            diskPayload[i] = entries[i].payload;
            codec[i] = spak::kCodecNone;
        }
        compSize[i] = (unsigned int)diskPayload[i].size();
    }

    // Assign sector-aligned disk offsets.
    const unsigned int tocOffset = spak::kHeaderBytes;
    unsigned int cursor = tocOffset + count * spak::kEntryBytes;
    cursor = ((cursor + sector - 1) / sector) * sector;
    std::vector<unsigned int> diskOffset(count);
    for (unsigned int i = 0; i < count; ++i)
    {
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
    for (unsigned int i = 0; i < count; ++i)
    {
        file.resize(diskOffset[i], 0);
        file.insert(file.end(), diskPayload[i].begin(), diskPayload[i].end());
    }
    file.resize(((file.size() + sector - 1) / sector) * sector, 0);

    if (!WriteFileBytes(outPath, &file[0], file.size()))
    { fprintf(stderr, "spakc: cannot write %s\n", outPath.c_str()); return false; }

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
} // namespace

int main(int argc, char** argv)
{
    if (argc >= 2 && std::string(argv[1]) == "build")
    {
        if (argc < 5)
        { fprintf(stderr, "usage: spakc build <out.spak> <contentRoot> <meshRel...> [--raw]\n"); return 2; }
        const std::string outSpak = argv[2];
        const std::string root    = argv[3];
        bool compress = true;
        std::vector<std::string> meshes;
        for (int i = 4; i < argc; ++i)
        {
            if (std::string(argv[i]) == "--raw") compress = false;
            else meshes.push_back(argv[i]);
        }
        if (meshes.empty()) { fprintf(stderr, "spakc: no meshes given\n"); return 2; }
        if (!ResolveBundler()) return 1;
        g_tmpBase = outSpak;

        const std::string rootAbs = AbsPath(root);
        std::vector<Entry> entries;
        std::set<unsigned int> seenTex;
        int okMeshes = 0;
        for (size_t i = 0; i < meshes.size(); ++i)
            if (AddMesh(entries, seenTex, rootAbs, meshes[i])) ++okMeshes;
        if (okMeshes == 0) { fprintf(stderr, "spakc: no meshes cooked\n"); return 1; }
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
