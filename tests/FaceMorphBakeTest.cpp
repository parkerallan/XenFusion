// Bakes a hand-written glTF carrying blendshapes and checks the whole morph
// contract the console depends on: ARKit name resolution (including the
// qualified and separator-happy spellings exporters emit), non-ARKit targets
// being dropped rather than shipped, the affected vertices being permuted into
// one contiguous range, and a deformation reproducing the authored shape within
// quantisation error.
//
// The fixture is written at run time rather than committed: the point is the
// pipeline, and a two-triangle mesh states the expectations far more plainly
// than a real character would.

#include "anim/FaceDeform.h"
#include "anim/FaceShapes.h"
#include "render/Mesh.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace
{
    int Fail(const std::string& message)
    {
        std::cerr << "FaceMorphBakeTest: " << message << '\n';
        return 1;
    }

    void PushFloats(std::vector<uint8_t>& buffer, const std::vector<float>& values)
    {
        const uint8_t* bytes = reinterpret_cast<const uint8_t*>(values.data());
        buffer.insert(buffer.end(), bytes, bytes + values.size() * sizeof(float));
    }

    std::string Base64(const std::vector<uint8_t>& data)
    {
        static const char* kAlphabet =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        for (size_t i = 0; i < data.size(); i += 3)
        {
            const uint32_t remaining = (uint32_t)(data.size() - i);
            const uint32_t triple = ((uint32_t)data[i] << 16) |
                                    (remaining > 1 ? ((uint32_t)data[i + 1] << 8) : 0u) |
                                    (remaining > 2 ?  (uint32_t)data[i + 2] : 0u);
            out += kAlphabet[(triple >> 18) & 0x3F];
            out += kAlphabet[(triple >> 12) & 0x3F];
            out += remaining > 1 ? kAlphabet[(triple >> 6) & 0x3F] : '=';
            out += remaining > 2 ? kAlphabet[triple & 0x3F] : '=';
        }
        return out;
    }

    // Four vertices, two triangles, one joint. The upper pair never moves; the
    // lower pair is what the blendshapes drive.
    const float kJawDrop = -0.5f;
    const float kSmileX  = 0.25f;

    bool WriteFixture(const fs::path& path)
    {
        std::vector<uint8_t> buffer;

        const size_t positionOffset = buffer.size();
        PushFloats(buffer, {-1.0f,  1.0f, 0.0f,   1.0f,  1.0f, 0.0f,
                            -1.0f, -1.0f, 0.0f,   1.0f, -1.0f, 0.0f});
        const size_t normalOffset = buffer.size();
        PushFloats(buffer, {0.0f, 0.0f, 1.0f,  0.0f, 0.0f, 1.0f,
                            0.0f, 0.0f, 1.0f,  0.0f, 0.0f, 1.0f});
        // jawOpen: the lower pair drops.
        const size_t jawOffset = buffer.size();
        PushFloats(buffer, {0.0f, 0.0f, 0.0f,  0.0f, 0.0f, 0.0f,
                            0.0f, kJawDrop, 0.0f,  0.0f, kJawDrop, 0.0f});
        // Mouth_Smile_Left: one lower corner pulls sideways.
        const size_t smileOffset = buffer.size();
        PushFloats(buffer, {0.0f, 0.0f, 0.0f,  0.0f, 0.0f, 0.0f,
                            0.0f, 0.0f, 0.0f,  kSmileX, 0.0f, 0.0f});
        // A target no ARKit shape answers to. It moves an UPPER vertex, so if the
        // baker were to keep it the morph region would grow to cover that vertex
        // and this test would notice.
        const size_t customOffset = buffer.size();
        PushFloats(buffer, {0.0f, 0.5f, 0.0f,  0.0f, 0.0f, 0.0f,
                            0.0f, 0.0f, 0.0f,  0.0f, 0.0f, 0.0f});

        const size_t jointOffset = buffer.size();
        for (int vertex = 0; vertex < 4; ++vertex)
            for (int influence = 0; influence < 4; ++influence)
                buffer.push_back(0);
        const size_t weightOffset = buffer.size();
        PushFloats(buffer, {1.0f, 0.0f, 0.0f, 0.0f,  1.0f, 0.0f, 0.0f, 0.0f,
                            1.0f, 0.0f, 0.0f, 0.0f,  1.0f, 0.0f, 0.0f, 0.0f});
        const size_t inverseBindOffset = buffer.size();
        PushFloats(buffer, {1.0f, 0.0f, 0.0f, 0.0f,  0.0f, 1.0f, 0.0f, 0.0f,
                            0.0f, 0.0f, 1.0f, 0.0f,  0.0f, 0.0f, 0.0f, 1.0f});
        const size_t indexOffset = buffer.size();
        const uint16_t indices[6] = {0, 2, 1, 1, 2, 3};
        const uint8_t* indexBytes = reinterpret_cast<const uint8_t*>(indices);
        buffer.insert(buffer.end(), indexBytes, indexBytes + sizeof(indices));

        std::ofstream out(path);
        if (!out)
            return false;
        out <<
R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],
"nodes":[{"name":"Root","children":[1,2]},{"name":"Joint"},{"name":"Head","mesh":0,"skin":0}],
"skins":[{"joints":[1],"inverseBindMatrices":5}],
"meshes":[{"name":"Head","primitives":[{"attributes":{"POSITION":0,"NORMAL":1,"JOINTS_0":3,"WEIGHTS_0":4},
"targets":[{"POSITION":6},{"POSITION":7},{"POSITION":8}],"indices":2}],
"extras":{"targetNames":["jawOpen","Mouth_Smile_Left","SomethingCustom"]}}],
"accessors":[
{"bufferView":0,"componentType":5126,"count":4,"type":"VEC3","min":[-1,-1,0],"max":[1,1,0]},
{"bufferView":1,"componentType":5126,"count":4,"type":"VEC3"},
{"bufferView":8,"componentType":5123,"count":6,"type":"SCALAR"},
{"bufferView":5,"componentType":5121,"count":4,"type":"VEC4"},
{"bufferView":6,"componentType":5126,"count":4,"type":"VEC4"},
{"bufferView":7,"componentType":5126,"count":1,"type":"MAT4"},
{"bufferView":2,"componentType":5126,"count":4,"type":"VEC3","min":[0,)" << kJawDrop << R"(,0],"max":[0,0,0]},
{"bufferView":3,"componentType":5126,"count":4,"type":"VEC3","min":[0,0,0],"max":[)" << kSmileX << R"(,0,0]},
{"bufferView":4,"componentType":5126,"count":4,"type":"VEC3","min":[0,0,0],"max":[0,0.5,0]}],
"bufferViews":[
{"buffer":0,"byteOffset":)" << positionOffset << R"(,"byteLength":48},
{"buffer":0,"byteOffset":)" << normalOffset << R"(,"byteLength":48},
{"buffer":0,"byteOffset":)" << jawOffset << R"(,"byteLength":48},
{"buffer":0,"byteOffset":)" << smileOffset << R"(,"byteLength":48},
{"buffer":0,"byteOffset":)" << customOffset << R"(,"byteLength":48},
{"buffer":0,"byteOffset":)" << jointOffset << R"(,"byteLength":16},
{"buffer":0,"byteOffset":)" << weightOffset << R"(,"byteLength":64},
{"buffer":0,"byteOffset":)" << inverseBindOffset << R"(,"byteLength":64},
{"buffer":0,"byteOffset":)" << indexOffset << R"(,"byteLength":12}],
"buffers":[{"byteLength":)" << buffer.size() << R"(,"uri":"data:application/octet-stream;base64,)"
            << Base64(buffer) << R"("}]})";
        return !!out;
    }

    // The blob's morph block, read back exactly the way both runtimes read it.
    struct BakedMorph
    {
        uint32_t firstVertex = 0;
        uint32_t vertexCount = 0;
        std::vector<MeshMorphTarget> targets;
    };

    bool ReadString(std::ifstream& in, std::string& value)
    {
        uint32_t length = 0;
        in.read(reinterpret_cast<char*>(&length), sizeof(length));
        if (!in || length >= 4096)
            return false;
        value.resize(length);
        if (length)
            in.read(value.data(), length);
        return !!in;
    }

    bool ReadBlob(const fs::path& path, MeshHeader& header,
                  std::vector<MeshVertex>& vertices, std::vector<uint32_t>& indices,
                  BakedMorph& morph)
    {
        std::ifstream in(path, std::ios::binary);
        if (!in)
            return false;
        in.read(reinterpret_cast<char*>(&header), sizeof(header));
        if (!in || std::memcmp(header.magic, "M360", 4) != 0)
            return false;
        vertices.resize(header.vertexCount);
        indices.resize(header.indexCount);
        in.read(reinterpret_cast<char*>(vertices.data()),
                (std::streamsize)(vertices.size() * sizeof(MeshVertex)));
        in.read(reinterpret_cast<char*>(indices.data()),
                (std::streamsize)(indices.size() * sizeof(uint32_t)));

        uint32_t subsetCount = 0;
        in.read(reinterpret_cast<char*>(&subsetCount), sizeof(subsetCount));
        for (uint32_t i = 0; i < subsetCount; ++i)
        {
            in.seekg(8, std::ios::cur);
            for (int slot = 0; slot < 7; ++slot)
            {
                std::string ignored;
                if (!ReadString(in, ignored))
                    return false;
            }
        }
        if ((header.flags & MESH_FLAG_SKINNED) != 0)
        {
            in.seekg((std::streamoff)header.vertexCount * 8, std::ios::cur);
            for (uint32_t joint = 0; joint < header.jointCount; ++joint)
            {
                std::string name;
                if (!ReadString(in, name))
                    return false;
                in.seekg(4 + 64 + 64, std::ios::cur);
            }
        }
        if ((header.flags & MESH_FLAG_MORPH) == 0)
            return !!in;

        uint32_t targetCount = 0;
        in.read(reinterpret_cast<char*>(&targetCount), sizeof(targetCount));
        in.read(reinterpret_cast<char*>(&morph.firstVertex), sizeof(morph.firstVertex));
        in.read(reinterpret_cast<char*>(&morph.vertexCount), sizeof(morph.vertexCount));
        morph.targets.resize(targetCount);
        for (MeshMorphTarget& target : morph.targets)
        {
            uint32_t deltaCount = 0;
            if (!ReadString(in, target.name))
                return false;
            in.read(reinterpret_cast<char*>(&target.shape), sizeof(target.shape));
            in.read(reinterpret_cast<char*>(&target.positionScale), sizeof(target.positionScale));
            in.read(reinterpret_cast<char*>(&deltaCount), sizeof(deltaCount));
            target.deltas.resize(deltaCount);
            in.read(reinterpret_cast<char*>(target.deltas.data()),
                    (std::streamsize)(target.deltas.size() * sizeof(face::MorphDelta)));
        }
        return !!in;
    }
}

int main()
{
    const fs::path dir = fs::temp_directory_path() / "xenfusion_face_morph_test";
    std::error_code ec;
    fs::create_directories(dir, ec);
    const fs::path source = dir / "head.gltf";
    const fs::path baked  = dir / "head.mesh";
    if (!WriteFixture(source))
        return Fail("could not write the glTF fixture");

    std::string error;
    if (!mesh::BakeModel(source, baked, error))
        return Fail("bake failed: " + error);

    MeshHeader header{};
    std::vector<MeshVertex> vertices;
    std::vector<uint32_t> indices;
    BakedMorph morph;
    if (!ReadBlob(baked, header, vertices, indices, morph))
        return Fail("could not read the baked blob");

    if (header.version != MESH_VERSION)
        return Fail("blob version is not MESH_VERSION");
    if ((header.flags & MESH_FLAG_SKINNED) == 0)
        return Fail("fixture did not bake as skinned, so morph capture never ran");
    if ((header.flags & MESH_FLAG_MORPH) == 0)
        return Fail("MESH_FLAG_MORPH is not set");

    // Only the two ARKit targets survive: a shape the face layer cannot drive is
    // payload the console would carry for nothing.
    if (morph.targets.size() != 2)
        return Fail("expected 2 ARKit targets, got " + std::to_string(morph.targets.size()));
    const MeshMorphTarget* jaw = nullptr;
    const MeshMorphTarget* smile = nullptr;
    for (const MeshMorphTarget& target : morph.targets)
    {
        if (target.shape == face::ShapeIndex("jawOpen")) jaw = &target;
        if (target.shape == face::ShapeIndex("mouthSmileLeft")) smile = &target;
        if (target.shape == face::kShapeNone)
            return Fail("an unresolved target reached the blob: " + target.name);
    }
    if (!jaw)
        return Fail("jawOpen did not resolve to its ARKit index");
    if (!smile)
        return Fail("'Mouth_Smile_Left' did not fold onto mouthSmileLeft");

    // The region covers exactly the vertices the surviving targets move: the two
    // lower corners, and nothing the dropped target touched.
    if (morph.vertexCount != 2)
        return Fail("morph region covers " + std::to_string(morph.vertexCount) +
                    " vertices, expected 2");
    if (morph.firstVertex + morph.vertexCount != header.vertexCount)
        return Fail("morph region is not the contiguous tail of the vertex buffer");
    if (jaw->deltas.size() != 2 || smile->deltas.size() != 1)
        return Fail("sparsification kept the wrong number of deltas");

    // Every index still points at a real vertex after the permutation, and the
    // triangles still reference all four.
    std::set<uint32_t> referenced;
    for (uint32_t index : indices)
    {
        if (index >= header.vertexCount)
            return Fail("an index escaped the vertex range after the permutation");
        referenced.insert(index);
    }
    if (referenced.size() != header.vertexCount)
        return Fail("the permutation orphaned a vertex");

    // Deform with jawOpen fully open and check the region lands where the shape
    // was authored. Y is untouched by the left-handed conversion, so the drop
    // survives import unchanged.
    std::vector<face::MorphTarget> views(morph.targets.size());
    for (size_t t = 0; t < morph.targets.size(); ++t)
    {
        views[t].deltas        = morph.targets[t].deltas.data();
        views[t].deltaCount    = (unsigned)morph.targets[t].deltas.size();
        views[t].positionScale = morph.targets[t].positionScale;
        views[t].shape         = morph.targets[t].shape;
    }
    face::MorphView view;
    view.targets     = views.data();
    view.targetCount = (unsigned)views.size();
    view.firstVertex = morph.firstVertex;
    view.vertexCount = morph.vertexCount;

    float weights[face::kShapeCount] = {};
    if (face::AnyActive(view, weights, face::kShapeCount))
        return Fail("a face at rest reported itself active, so it would take a clone for nothing");
    weights[face::ShapeIndex("jawOpen")] = 1.0f;
    if (!face::AnyActive(view, weights, face::kShapeCount))
        return Fail("an open jaw did not report the face as active");

    const unsigned stride = (unsigned)sizeof(MeshVertex);
    const unsigned char* base =
        reinterpret_cast<const unsigned char*>(&vertices[morph.firstVertex]);
    std::vector<unsigned char> deformed((size_t)view.vertexCount * stride);
    face::Deform(base, stride, view, weights, face::kShapeCount, deformed.data());

    for (unsigned v = 0; v < view.vertexCount; ++v)
    {
        const MeshVertex& before = vertices[morph.firstVertex + v];
        const MeshVertex& after  = *reinterpret_cast<const MeshVertex*>(&deformed[(size_t)v * stride]);
        const float dropped = after.py - before.py;
        if (std::fabs(dropped - kJawDrop) > 1e-3f)
            return Fail("vertex " + std::to_string(v) + " dropped by " + std::to_string(dropped) +
                        ", expected " + std::to_string(kJawDrop));
        if (std::fabs(after.px - before.px) > 1e-3f || std::fabs(after.pz - before.pz) > 1e-3f)
            return Fail("jawOpen moved a vertex off its own axis");
    }

    // Half weight, half the movement: the deformer is linear in the weight.
    weights[face::ShapeIndex("jawOpen")] = 0.5f;
    face::Deform(base, stride, view, weights, face::kShapeCount, deformed.data());
    {
        const MeshVertex& before = vertices[morph.firstVertex];
        const MeshVertex& after  = *reinterpret_cast<const MeshVertex*>(deformed.data());
        if (std::fabs((after.py - before.py) - kJawDrop * 0.5f) > 1e-3f)
            return Fail("a half-weight jawOpen did not produce half the movement");
    }

    // The cook's byte order. A delta is u16 + 3*i16 + 4*i8, and spakc writes the
    // 16-bit fields big-endian so the console reads the record natively with no
    // conversion at all. Getting this wrong is invisible on PC and produces a
    // face that explodes on hardware, so assert the contract directly: swap the
    // record the way the cooker does, then decode it big-endian and require the
    // authored values back.
    for (const face::MorphDelta& authored : jaw->deltas)
    {
        const uint8_t* source = reinterpret_cast<const uint8_t*>(&authored);
        uint8_t cooked[12];
        for (int field = 0; field < 4; ++field)
        {
            cooked[field * 2 + 0] = source[field * 2 + 1];
            cooked[field * 2 + 1] = source[field * 2 + 0];
        }
        for (int byte = 8; byte < 12; ++byte)
            cooked[byte] = source[byte];

        auto readBE16 = [&cooked](int at) {
            return (uint16_t)(((uint16_t)cooked[at] << 8) | (uint16_t)cooked[at + 1]);
        };
        if (readBE16(0) != authored.vertex)
            return Fail("cooked delta vertex id does not survive the big-endian swap");
        if ((int16_t)readBE16(2) != authored.px || (int16_t)readBE16(4) != authored.py ||
            (int16_t)readBE16(6) != authored.pz)
            return Fail("cooked delta position does not survive the big-endian swap");
        if ((int8_t)cooked[8] != authored.nx || (int8_t)cooked[9] != authored.ny ||
            (int8_t)cooked[10] != authored.nz)
            return Fail("cooked delta normal bytes must NOT be swapped");
    }

    fs::remove_all(dir, ec);
    std::cout << "FaceMorphBakeTest: ok (" << morph.targets.size() << " ARKit targets, region "
              << morph.vertexCount << " vertices)\n";
    return 0;
}
