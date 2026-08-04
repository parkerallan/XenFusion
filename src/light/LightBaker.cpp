#include "light/LightBaker.h"

#include "light/LightBasis.h"
#include "light/LightSelect.h"
#include "light/LightmapUnwrap.h"
#include "render/Mesh.h"

#include <btBulletDynamicsCommon.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <memory>
#include <random>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace
{
    const float kPi = 3.14159265358979323846f;

    struct Vec3
    {
        float x = 0.0f, y = 0.0f, z = 0.0f;
        Vec3 operator+(const Vec3& r) const { return {x + r.x, y + r.y, z + r.z}; }
        Vec3 operator-(const Vec3& r) const { return {x - r.x, y - r.y, z - r.z}; }
        Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
    };

    float Dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
    Vec3 Cross(const Vec3& a, const Vec3& b)
    {
        return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
    }
    Vec3 Normalize(const Vec3& value)
    {
        const float lengthSq = Dot(value, value);
        return lengthSq > 1.0e-12f ? value * (1.0f / std::sqrt(lengthSq)) : Vec3{0.0f, 1.0f, 0.0f};
    }

    struct Matrix
    {
        float m[16] = {};
    };

    Matrix Identity()
    {
        Matrix value;
        value.m[0] = value.m[5] = value.m[10] = value.m[15] = 1.0f;
        return value;
    }
    Matrix Multiply(const Matrix& a, const Matrix& b)
    {
        Matrix result;
        for (int row = 0; row < 4; ++row)
            for (int column = 0; column < 4; ++column)
                for (int inner = 0; inner < 4; ++inner)
                    result.m[row * 4 + column] += a.m[row * 4 + inner] * b.m[inner * 4 + column];
        return result;
    }
    Matrix Compose(const SceneObject& object)
    {
        const float x = object.rotation[0] * kPi / 180.0f;
        const float y = object.rotation[1] * kPi / 180.0f;
        const float z = object.rotation[2] * kPi / 180.0f;
        Matrix scale = Identity(), rx = Identity(), ry = Identity(), rz = Identity(), translation = Identity();
        scale.m[0] = object.scale[0]; scale.m[5] = object.scale[1]; scale.m[10] = object.scale[2];
        rx.m[5] = std::cos(x); rx.m[6] = std::sin(x); rx.m[9] = -std::sin(x); rx.m[10] = std::cos(x);
        ry.m[0] = std::cos(y); ry.m[2] = -std::sin(y); ry.m[8] = std::sin(y); ry.m[10] = std::cos(y);
        rz.m[0] = std::cos(z); rz.m[1] = std::sin(z); rz.m[4] = -std::sin(z); rz.m[5] = std::cos(z);
        translation.m[12] = object.position[0]; translation.m[13] = object.position[1]; translation.m[14] = object.position[2];
        return Multiply(Multiply(Multiply(Multiply(scale, rx), ry), rz), translation);
    }
    Vec3 TransformPoint(const Matrix& matrix, const Vec3& point)
    {
        return {point.x * matrix.m[0] + point.y * matrix.m[4] + point.z * matrix.m[8] + matrix.m[12],
                point.x * matrix.m[1] + point.y * matrix.m[5] + point.z * matrix.m[9] + matrix.m[13],
                point.x * matrix.m[2] + point.y * matrix.m[6] + point.z * matrix.m[10] + matrix.m[14]};
    }
    Vec3 TransformNormal(const Matrix& matrix, const Vec3& normal)
    {
        return Normalize({normal.x * matrix.m[0] + normal.y * matrix.m[4] + normal.z * matrix.m[8],
                          normal.x * matrix.m[1] + normal.y * matrix.m[5] + normal.z * matrix.m[9],
                          normal.x * matrix.m[2] + normal.y * matrix.m[6] + normal.z * matrix.m[10]});
    }

    std::string LowerExtension(const fs::path& path)
    {
        std::string extension = path.extension().string();
        for (char& c : extension) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return extension;
    }
    fs::path FindSource(const fs::path& input)
    {
        if (LowerExtension(input) != ".mesh") return input;
        static const char* extensions[] = {".gltf", ".glb", ".fbx", ".obj", ".dae", ".3ds", ".ply", ".stl"};
        for (const char* extension : extensions)
        {
            fs::path candidate = input;
            candidate.replace_extension(extension);
            std::error_code ec;
            if (fs::exists(candidate, ec)) return candidate;
        }
        return {};
    }

    struct CpuMesh
    {
        std::vector<MeshVertex> vertices;
        std::vector<uint32_t> indices;
    };
    bool LoadMesh(const fs::path& path, CpuMesh& mesh, std::string& error)
    {
        std::ifstream input(path, std::ios::binary);
        MeshHeader header = {};
        input.read(reinterpret_cast<char*>(&header), sizeof(header));
        if (!input || std::memcmp(header.magic, "M360", 4) != 0 || header.version != MESH_VERSION)
        {
            error = "missing or stale mesh v9 blob: " + path.string();
            return false;
        }
        if ((header.flags & MESH_FLAG_SKINNED) != 0)
        {
            error = "skinned meshes use probes and cannot be lightmapped: " + path.string();
            return false;
        }
        mesh.vertices.resize(header.vertexCount);
        mesh.indices.resize(header.indexCount);
        input.read(reinterpret_cast<char*>(mesh.vertices.data()), static_cast<std::streamsize>(mesh.vertices.size() * sizeof(MeshVertex)));
        input.read(reinterpret_cast<char*>(mesh.indices.data()), static_cast<std::streamsize>(mesh.indices.size() * sizeof(uint32_t)));
        if (!input)
        {
            error = "truncated mesh blob: " + path.string();
            return false;
        }
        return true;
    }

    struct Instance
    {
        uint32_t objectIndex = 0;
        bool castShadow = false;
        fs::path meshPath, sourcePath, uvPath;
        Matrix world;
        CpuMesh mesh;
        lightmap::UvMesh uv;
        uint32_t atlasX = 0, atlasY = 0, atlasWidth = 0, atlasHeight = 0;
        std::vector<Vec3> worldPositions;
        std::vector<Vec3> worldNormals;
        std::unique_ptr<btTriangleMesh> triangles;
        std::unique_ptr<btBvhTriangleMeshShape> shape;
        std::unique_ptr<btCollisionObject> collision;
        std::unique_ptr<btCollisionObject> shadowCollision;
        float meanLight[3] = {};
    };

    struct Texel
    {
        bool covered = false;
        uint32_t instance = 0;
        Vec3 position, normal;
        lbasis::Accumulator basis;
    };

    struct BakeLight
    {
        enum Type { Directional, Point, Spot } type;
        Vec3 position, direction;
        float color[3] = {};
        float range = 1.0f, innerCos = 1.0f, outerCos = 0.0f;
    };

    class RayScene
    {
    public:
        RayScene() : dispatcher(&configuration), world(&dispatcher, &broadphase, &configuration) {}
        void Add(btCollisionObject* object) { world.addCollisionObject(object); }
        bool Visible(const Vec3& start, const Vec3& end, int* objectIndex = nullptr) const
        {
            btCollisionWorld::ClosestRayResultCallback hit(btVector3(start.x, start.y, start.z), btVector3(end.x, end.y, end.z));
            world.rayTest(hit.m_rayFromWorld, hit.m_rayToWorld, hit);
            if (!hit.hasHit()) return true;
            if (objectIndex) *objectIndex = hit.m_collisionObject->getUserIndex();
            return false;
        }
    private:
        btDefaultCollisionConfiguration configuration;
        btCollisionDispatcher dispatcher;
        btDbvtBroadphase broadphase;
        btCollisionWorld world;
    };

    Vec3 Hemisphere(const Vec3& normal, std::mt19937& random)
    {
        std::uniform_real_distribution<float> unit(0.0f, 1.0f);
        const float radius = std::sqrt(unit(random));
        const float angle = 2.0f * kPi * unit(random);
        const Vec3 tangent = Normalize(std::fabs(normal.y) < 0.99f ? Cross({0, 1, 0}, normal) : Cross({1, 0, 0}, normal));
        const Vec3 bitangent = Cross(normal, tangent);
        return Normalize(tangent * (radius * std::cos(angle)) + bitangent * (radius * std::sin(angle)) +
                         normal * std::sqrt((std::max)(0.0f, 1.0f - radius * radius)));
    }

    void AddSample(Texel& texel, const float color[3], const Vec3& direction, float weight)
    {
        lbasis::Sample sample = {};
        sample.color[0] = color[0]; sample.color[1] = color[1]; sample.color[2] = color[2];
        sample.direction[0] = direction.x; sample.direction[1] = direction.y; sample.direction[2] = direction.z;
        sample.weight = weight;
        texel.basis.Add(sample);
    }

    uint8_t Byte(float value)
    {
        return static_cast<uint8_t>((std::max)(0.0f, (std::min)(255.0f, value * 255.0f + 0.5f)));
    }

    template<typename T> void Write(std::ofstream& output, const T& value)
    {
        output.write(reinterpret_cast<const char*>(&value), sizeof(value));
    }
}

namespace lightmap
{
    bool BakeScene(const fs::path& projectRoot, const SceneFile& scene, const BakeOptions& options,
                   BakeResult& result, std::string& error, const ProgressFn& progress)
    {
        result = {};
        if (options.atlasSize == 0 || options.directionSize == 0 || options.directionSize > options.atlasSize)
        {
            error = "invalid lightmap dimensions";
            return false;
        }
        auto report = [&](const std::string& text) { if (progress) progress(text); };
        std::vector<Instance> instances;
        std::vector<BakeLight> lights;
        float environment[3] = {};

        report("Collecting static geometry");
        for (uint32_t objectIndex = 0; objectIndex < scene.objects.size(); ++objectIndex)
        {
            const SceneObject& object = scene.objects[objectIndex];
            if (!object.visible) continue;
            const ObjectAttribute* model = nullptr;
            bool movable = false;
            for (const ObjectAttribute& attribute : object.attributes)
            {
                if (attribute.type == "3D Model" && !attribute.model_path.empty() && !model) model = &attribute;
                if (attribute.type == "Animator") movable = true;
                if (attribute.type == "Rigid Body" && attribute.phys_kind != 0) movable = true;
                if (attribute.light_mode == lsel::Realtime) continue;
                if (attribute.type == "Environment Light")
                    for (int axis = 0; axis < 3; ++axis) environment[axis] += attribute.light_color[axis] * attribute.light_intensity;
                if (attribute.type == "Directional Light" || attribute.type == "Point Light" || attribute.type == "Spot Light")
                {
                    BakeLight light;
                    light.type = attribute.type == "Directional Light" ? BakeLight::Directional :
                                 attribute.type == "Point Light" ? BakeLight::Point : BakeLight::Spot;
                    light.position = {object.position[0], object.position[1], object.position[2]};
                    const Matrix transform = Compose(object);
                    light.direction = Normalize({transform.m[8], transform.m[9], transform.m[10]});
                    for (int axis = 0; axis < 3; ++axis) light.color[axis] = attribute.light_color[axis] * attribute.light_intensity;
                    light.range = (std::max)(0.1f, attribute.light_range);
                    light.innerCos = std::cos(attribute.light_inner_deg * kPi / 180.0f);
                    light.outerCos = std::cos(attribute.light_outer_deg * kPi / 180.0f);
                    lights.push_back(light);
                }
            }
            if (!model || movable) continue;

            Instance instance;
            instance.objectIndex = objectIndex;
            instance.castShadow = model->cast_shadow;
            instance.world = Compose(object);
            instance.meshPath = projectRoot / model->model_path;
            if (LowerExtension(instance.meshPath) != ".mesh") instance.meshPath.replace_extension(".mesh");
            instance.sourcePath = FindSource(projectRoot / model->model_path);
            if (instance.sourcePath.empty()) instance.sourcePath = FindSource(instance.meshPath);
            if (instance.sourcePath.empty())
            {
                error = "lightmap unwrap needs a source model beside " + instance.meshPath.string();
                return false;
            }
            instance.uvPath = instance.meshPath;
            instance.uvPath.replace_extension(".lmuv");
            if (!LoadMesh(instance.meshPath, instance.mesh, error)) return false;
            std::string unwrapError;
            if (!EnsureUvSidecar(instance.sourcePath, instance.uvPath, instance.uv, unwrapError))
            {
                error = "unwrap failed for " + instance.sourcePath.string() + ": " + unwrapError;
                return false;
            }
            if (instance.uv.sourceVertexCount != instance.mesh.vertices.size())
            {
                error = "LMUV source vertex count does not match mesh v9: " + instance.sourcePath.string();
                return false;
            }
            instances.push_back(std::move(instance));
        }
        if (instances.empty())
        {
            error = "scene has no visible static mesh instances";
            return false;
        }

        report("Packing lightmap atlas");
        uint32_t cursorX = 4, cursorY = 4, rowHeight = 0;
        for (Instance& instance : instances)
        {
            const float allocationScale = (std::min)(1.0f, 256.0f / (std::max)(instance.uv.width, instance.uv.height));
            instance.atlasWidth = (std::max)(8u, static_cast<uint32_t>(std::ceil(instance.uv.width * allocationScale)));
            instance.atlasHeight = (std::max)(8u, static_cast<uint32_t>(std::ceil(instance.uv.height * allocationScale)));
            if (cursorX + instance.atlasWidth + 4 > options.atlasSize)
            {
                cursorX = 4;
                cursorY += rowHeight + 4;
                rowHeight = 0;
            }
            if (cursorY + instance.atlasHeight + 4 > options.atlasSize)
            {
                error = "lightmap atlas overflow; split the scene or reduce texel density";
                return false;
            }
            instance.atlasX = cursorX; instance.atlasY = cursorY;
            cursorX += instance.atlasWidth + 4;
            rowHeight = (std::max)(rowHeight, instance.atlasHeight);
        }

        const size_t texelCount = static_cast<size_t>(options.atlasSize) * options.atlasSize;
        std::vector<Texel> texels(texelCount);
        RayScene rayScene;
        RayScene shadowRayScene;
        Vec3 boundsMin{FLT_MAX, FLT_MAX, FLT_MAX};
        Vec3 boundsMax{-FLT_MAX, -FLT_MAX, -FLT_MAX};

        report("Rasterizing world-space surfels");
        for (uint32_t instanceIndex = 0; instanceIndex < instances.size(); ++instanceIndex)
        {
            Instance& instance = instances[instanceIndex];
            instance.worldPositions.resize(instance.mesh.vertices.size());
            instance.worldNormals.resize(instance.mesh.vertices.size());
            for (size_t vertexIndex = 0; vertexIndex < instance.mesh.vertices.size(); ++vertexIndex)
            {
                const MeshVertex& source = instance.mesh.vertices[vertexIndex];
                instance.worldPositions[vertexIndex] = TransformPoint(instance.world, {source.px, source.py, source.pz});
                instance.worldNormals[vertexIndex] = TransformNormal(instance.world, {source.nx, source.ny, source.nz});
                const Vec3& p = instance.worldPositions[vertexIndex];
                boundsMin.x = (std::min)(boundsMin.x, p.x); boundsMin.y = (std::min)(boundsMin.y, p.y); boundsMin.z = (std::min)(boundsMin.z, p.z);
                boundsMax.x = (std::max)(boundsMax.x, p.x); boundsMax.y = (std::max)(boundsMax.y, p.y); boundsMax.z = (std::max)(boundsMax.z, p.z);
            }
            instance.triangles.reset(new btTriangleMesh(true, false));
            for (size_t index = 0; index + 2 < instance.mesh.indices.size(); index += 3)
            {
                const Vec3& a = instance.worldPositions[instance.mesh.indices[index]];
                const Vec3& b = instance.worldPositions[instance.mesh.indices[index + 1]];
                const Vec3& c = instance.worldPositions[instance.mesh.indices[index + 2]];
                instance.triangles->addTriangle(btVector3(a.x, a.y, a.z), btVector3(b.x, b.y, b.z), btVector3(c.x, c.y, c.z), true);
            }
            instance.shape.reset(new btBvhTriangleMeshShape(instance.triangles.get(), true));
            instance.collision.reset(new btCollisionObject());
            instance.collision->setCollisionShape(instance.shape.get());
            instance.collision->setUserIndex(static_cast<int>(instanceIndex));
            rayScene.Add(instance.collision.get());
            if (instance.castShadow)
            {
                instance.shadowCollision.reset(new btCollisionObject());
                instance.shadowCollision->setCollisionShape(instance.shape.get());
                instance.shadowCollision->setUserIndex(static_cast<int>(instanceIndex));
                shadowRayScene.Add(instance.shadowCollision.get());
            }

            for (size_t index = 0; index + 2 < instance.uv.indices.size(); index += 3)
            {
                const UvVertex& ua = instance.uv.vertices[instance.uv.indices[index]];
                const UvVertex& ub = instance.uv.vertices[instance.uv.indices[index + 1]];
                const UvVertex& uc = instance.uv.vertices[instance.uv.indices[index + 2]];
                const float ax = instance.atlasX + ua.u * instance.atlasWidth, ay = instance.atlasY + ua.v * instance.atlasHeight;
                const float bx = instance.atlasX + ub.u * instance.atlasWidth, by = instance.atlasY + ub.v * instance.atlasHeight;
                const float cx = instance.atlasX + uc.u * instance.atlasWidth, cy = instance.atlasY + uc.v * instance.atlasHeight;
                const float area = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
                if (std::fabs(area) < 1.0e-6f) continue;
                const int minX = (std::max)(0, static_cast<int>(std::floor((std::min)({ax, bx, cx}))));
                const int maxX = (std::min)(static_cast<int>(options.atlasSize) - 1, static_cast<int>(std::ceil((std::max)({ax, bx, cx}))));
                const int minY = (std::max)(0, static_cast<int>(std::floor((std::min)({ay, by, cy}))));
                const int maxY = (std::min)(static_cast<int>(options.atlasSize) - 1, static_cast<int>(std::ceil((std::max)({ay, by, cy}))));
                for (int y = minY; y <= maxY; ++y) for (int x = minX; x <= maxX; ++x)
                {
                    const float px = x + 0.5f, py = y + 0.5f;
                    const float wb = ((px - ax) * (cy - ay) - (py - ay) * (cx - ax)) / area;
                    const float wc = ((bx - ax) * (py - ay) - (by - ay) * (px - ax)) / area;
                    const float wa = 1.0f - wb - wc;
                    if (wa < -1.0e-4f || wb < -1.0e-4f || wc < -1.0e-4f) continue;
                    Texel& texel = texels[static_cast<size_t>(y) * options.atlasSize + x];
                    const Vec3& pa = instance.worldPositions[ua.sourceVertex];
                    const Vec3& pb = instance.worldPositions[ub.sourceVertex];
                    const Vec3& pc = instance.worldPositions[uc.sourceVertex];
                    const Vec3& na = instance.worldNormals[ua.sourceVertex];
                    const Vec3& nb = instance.worldNormals[ub.sourceVertex];
                    const Vec3& nc = instance.worldNormals[uc.sourceVertex];
                    texel.covered = true; texel.instance = instanceIndex;
                    texel.position = pa * wa + pb * wb + pc * wc;
                    texel.normal = Normalize(na * wa + nb * wb + nc * wc);
                }
            }
        }

        report("Tracing direct and environment lighting");
        std::mt19937 random(0x360u);
        for (Texel& texel : texels)
        {
            if (!texel.covered) continue;
            ++result.coveredTexels;
            for (const BakeLight& light : lights)
            {
                Vec3 direction;
                float attenuation = 1.0f;
                if (light.type == BakeLight::Directional)
                    direction = light.direction * -1.0f;
                else
                    direction = Normalize(light.position - texel.position);

                const Vec3 shadingNormal = Dot(texel.normal, direction) >= 0.0f
                    ? texel.normal : texel.normal * -1.0f;
                const Vec3 origin = texel.position + shadingNormal * 0.01f;
                const Vec3 endpoint = light.type == BakeLight::Directional
                    ? origin + direction * 10000.0f : light.position;
                if (light.type != BakeLight::Directional)
                {
                    direction = Normalize(light.position - origin);
                    if (light.type == BakeLight::Point)
                    {
                        lsel::PointLight point = {};
                        point.position[0] = light.position.x; point.position[1] = light.position.y; point.position[2] = light.position.z;
                        point.invRangeSq = 1.0f / (light.range * light.range);
                        const float sample[3] = {origin.x, origin.y, origin.z};
                        attenuation = lsel::PointAtten(point, sample);
                    }
                    else
                    {
                        lsel::SpotLight spot = {};
                        spot.position[0] = light.position.x; spot.position[1] = light.position.y; spot.position[2] = light.position.z;
                        spot.direction[0] = light.direction.x; spot.direction[1] = light.direction.y; spot.direction[2] = light.direction.z;
                        spot.invRange = 1.0f / light.range; spot.innerCos = light.innerCos; spot.outerCos = light.outerCos;
                        const float sample[3] = {origin.x, origin.y, origin.z};
                        attenuation = lsel::SpotAtten(spot, sample);
                    }
                }
                attenuation *= (std::max)(0.0f, Dot(shadingNormal, direction));
                if (attenuation > 0.0f && shadowRayScene.Visible(origin, endpoint)) AddSample(texel, light.color, direction, attenuation);
            }
        }

        std::vector<uint32_t> instanceSamples(instances.size(), 0);
        for (const Texel& texel : texels) if (texel.covered)
        {
            Instance& instance = instances[texel.instance];
            for (int channel = 0; channel < 3; ++channel) instance.meanLight[channel] += texel.basis.color[channel];
            ++instanceSamples[texel.instance];
        }
        for (size_t index = 0; index < instances.size(); ++index)
            if (instanceSamples[index]) for (float& channel : instances[index].meanLight) channel /= instanceSamples[index];

        report("Tracing one diffuse bounce");
        for (Texel& texel : texels) if (texel.covered)
        {
            const Vec3 origin = texel.position + texel.normal * 0.01f;
            for (unsigned ray = 0; ray < options.bounceRays; ++ray)
            {
                const Vec3 direction = Hemisphere(texel.normal, random);
                int hitInstance = -1;
                if (!rayScene.Visible(origin, origin + direction * 10000.0f, &hitInstance) && hitInstance >= 0)
                    AddSample(texel, instances[hitInstance].meanLight, direction,
                              0.2f / (std::max)(1u, options.bounceRays));
            }
        }

        report("Encoding lightmap textures");
        std::vector<uint8_t> color(texelCount * 4, 0), direction(texelCount * 4, 0), mask(texelCount, 0);
        for (size_t index = 0; index < texels.size(); ++index) if (texels[index].covered)
        {
            float linearColor[3], encodedColor[4], encodedDirection[4];
            lbasis::Encode(texels[index].basis, linearColor, encodedDirection);
            lbasis::EncodeRgbm(linearColor, encodedColor);
            color[index * 4] = Byte(encodedColor[0]); color[index * 4 + 1] = Byte(encodedColor[1]); color[index * 4 + 2] = Byte(encodedColor[2]); color[index * 4 + 3] = Byte(encodedColor[3]);
            direction[index * 4] = Byte(encodedDirection[0] * 0.5f + 0.5f);
            direction[index * 4 + 1] = Byte(encodedDirection[1] * 0.5f + 0.5f);
            direction[index * 4 + 2] = Byte(encodedDirection[2] * 0.5f + 0.5f);
            direction[index * 4 + 3] = Byte(encodedDirection[3]); mask[index] = 1;
        }
        for (int pass = 0; pass < 4; ++pass)
        {
            const std::vector<uint8_t> oldMask = mask;
            const std::vector<uint8_t> oldColor = color, oldDirection = direction;
            for (uint32_t y = 1; y + 1 < options.atlasSize; ++y) for (uint32_t x = 1; x + 1 < options.atlasSize; ++x)
            {
                const size_t index = static_cast<size_t>(y) * options.atlasSize + x;
                if (oldMask[index]) continue;
                const size_t neighbours[4] = {index - 1, index + 1, index - options.atlasSize, index + options.atlasSize};
                for (size_t neighbour : neighbours) if (oldMask[neighbour])
                {
                    std::memcpy(&color[index * 4], &oldColor[neighbour * 4], 4);
                    std::memcpy(&direction[index * 4], &oldDirection[neighbour * 4], 4);
                    mask[index] = 1; break;
                }
            }
        }
        const uint32_t downsample = options.atlasSize / options.directionSize;
        std::vector<uint8_t> smallDirection(static_cast<size_t>(options.directionSize) * options.directionSize * 4, 0);
        for (uint32_t y = 0; y < options.directionSize; ++y) for (uint32_t x = 0; x < options.directionSize; ++x)
            for (uint32_t channel = 0; channel < 4; ++channel)
            {
                uint32_t sum = 0;
                for (uint32_t dy = 0; dy < downsample; ++dy) for (uint32_t dx = 0; dx < downsample; ++dx)
                    sum += direction[((static_cast<size_t>(y * downsample + dy) * options.atlasSize + x * downsample + dx) * 4) + channel];
                smallDirection[(static_cast<size_t>(y) * options.directionSize + x) * 4 + channel] = static_cast<uint8_t>(sum / (downsample * downsample));
            }

        fs::path outputBase = scene.path;
        outputBase.replace_extension();
        result.colorPath = outputBase.string() + "_lm0.png";
        result.directionPath = outputBase.string() + "_lm1.png";
        result.metadataPath = outputBase.string() + ".lmap";
        if (!stbi_write_png(result.colorPath.string().c_str(), options.atlasSize, options.atlasSize, 4, color.data(), options.atlasSize * 4) ||
            !stbi_write_png(result.directionPath.string().c_str(), options.directionSize, options.directionSize, 4, smallDirection.data(), options.directionSize * 4))
        {
            error = "failed to write lightmap PNGs beside " + scene.path.string();
            return false;
        }

        report("Writing probes and metadata");
        struct Probe { float position[3]; float sh[12]; };
        std::vector<Probe> probes;
        const float spacing = (std::max)(0.1f, options.probeSpacing);
        const uint32_t dimensions[3] = {
            static_cast<uint32_t>(std::floor((boundsMax.x - boundsMin.x) / spacing)) + 1,
            static_cast<uint32_t>(std::floor((boundsMax.y - boundsMin.y) / spacing)) + 1,
            static_cast<uint32_t>(std::floor((boundsMax.z - boundsMin.z) / spacing)) + 1
        };
        const uint64_t probeTotal = static_cast<uint64_t>(dimensions[0]) * dimensions[1] * dimensions[2];
        if (probeTotal > 262144)
        {
            error = "probe grid exceeds 262144 samples; reduce scene bounds or increase spacing";
            return false;
        }
        probes.reserve(static_cast<size_t>(probeTotal));
        for (uint32_t z = 0; z < dimensions[2]; ++z) for (uint32_t y = 0; y < dimensions[1]; ++y) for (uint32_t x = 0; x < dimensions[0]; ++x)
        {
            Probe probe = {};
            const Vec3 position{boundsMin.x + x * spacing, boundsMin.y + y * spacing, boundsMin.z + z * spacing};
            probe.position[0] = position.x; probe.position[1] = position.y; probe.position[2] = position.z;
            for (int channel = 0; channel < 3; ++channel) probe.sh[channel] = environment[channel];
            for (const BakeLight& light : lights)
            {
                Vec3 incoming; float attenuation = 1.0f; Vec3 endpoint;
                if (light.type == BakeLight::Directional) { incoming = light.direction * -1.0f; endpoint = position + incoming * 10000.0f; }
                else
                {
                    incoming = Normalize(light.position - position); endpoint = light.position;
                    const float distance = std::sqrt(Dot(light.position - position, light.position - position));
                    attenuation = (std::max)(0.0f, 1.0f - distance / light.range); attenuation *= attenuation;
                }
                if (attenuation > 0.0f && shadowRayScene.Visible(position, endpoint)) for (int channel = 0; channel < 3; ++channel)
                {
                    const float value = light.color[channel] * attenuation;
                    probe.sh[channel] += value;
                    probe.sh[3 + channel] += value * incoming.x;
                    probe.sh[6 + channel] += value * incoming.y;
                    probe.sh[9 + channel] += value * incoming.z;
                }
            }
            probes.push_back(probe);
        }

        std::ofstream metadata(result.metadataPath, std::ios::binary | std::ios::trunc);
        if (!metadata) { error = "cannot write " + result.metadataPath.string(); return false; }
        metadata.write("LMP0", 4);
        const uint32_t version = 3, instanceCount = static_cast<uint32_t>(instances.size()), probeCount = static_cast<uint32_t>(probes.size());
        Write(metadata, version); Write(metadata, options.atlasSize); Write(metadata, options.directionSize); Write(metadata, instanceCount); Write(metadata, probeCount);
        Write(metadata, spacing); metadata.write(reinterpret_cast<const char*>(&boundsMin), sizeof(boundsMin));
        for (const Instance& instance : instances)
        {
            const float scaleOffset[4] = {static_cast<float>(instance.atlasWidth) / options.atlasSize,
                                          static_cast<float>(instance.atlasHeight) / options.atlasSize,
                                          static_cast<float>(instance.atlasX) / options.atlasSize,
                                          static_cast<float>(instance.atlasY) / options.atlasSize};
            Write(metadata, instance.objectIndex); metadata.write(reinterpret_cast<const char*>(scaleOffset), sizeof(scaleOffset));
        }
        if (!probes.empty()) metadata.write(reinterpret_cast<const char*>(probes.data()), static_cast<std::streamsize>(probes.size() * sizeof(Probe)));
        if (!metadata) { error = "failed while writing " + result.metadataPath.string(); return false; }
        result.staticInstanceCount = instanceCount;
        result.probeCount = probeCount;
        report("Lighting bake complete");
        return true;
    }
}