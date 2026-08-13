#include "scene/Hierarchy.h"

#include <cmath>
#include <iostream>
#include <string>
#include <vector>

namespace
{
    bool Near(float a, float b, float tolerance = 1.0e-4f)
    {
        return std::fabs(a - b) <= tolerance;
    }

    hier::Node MakeNode(float x, float y, float z, int parent)
    {
        hier::Node node;
        node.pos[0] = x; node.pos[1] = y; node.pos[2] = z;
        node.rot[0] = 17.0f; node.rot[1] = -23.0f; node.rot[2] = 31.0f;
        node.scale[0] = 1.25f; node.scale[1] = 0.75f; node.scale[2] = 2.0f;
        node.visible = true;
        node.parent = parent;
        return node;
    }

    bool MatNear(const float* a, const float* b, float tolerance = 1.0e-4f)
    {
        for (int i = 0; i < 16; ++i)
            if (!Near(a[i], b[i], tolerance)) return false;
        return true;
    }

    void Identity(float matrix[16])
    {
        for (int i = 0; i < 16; ++i) matrix[i] = 0.0f;
        matrix[0] = matrix[5] = matrix[10] = matrix[15] = 1.0f;
    }

    void Multiply(const float a[16], const float b[16], float out[16])
    {
        float result[16];
        for (int row = 0; row < 4; ++row)
            for (int column = 0; column < 4; ++column)
                result[row * 4 + column] =
                    a[row * 4] * b[column] + a[row * 4 + 1] * b[4 + column] +
                    a[row * 4 + 2] * b[8 + column] + a[row * 4 + 3] * b[12 + column];
        for (int i = 0; i < 16; ++i) out[i] = result[i];
    }

    void LegacyCompose(const hier::Node& node, float out[16])
    {
        const float d2r = 3.14159265f / 180.0f;
        const float x = node.rot[0] * d2r, y = node.rot[1] * d2r, z = node.rot[2] * d2r;
        float scale[16], rx[16], ry[16], rz[16], translation[16];
        Identity(scale); Identity(rx); Identity(ry); Identity(rz); Identity(translation);
        scale[0] = node.scale[0]; scale[5] = node.scale[1]; scale[10] = node.scale[2];
        rx[5] = std::cos(x); rx[6] = std::sin(x); rx[9] = -std::sin(x); rx[10] = std::cos(x);
        ry[0] = std::cos(y); ry[2] = -std::sin(y); ry[8] = std::sin(y); ry[10] = std::cos(y);
        rz[0] = std::cos(z); rz[1] = std::sin(z); rz[4] = -std::sin(z); rz[5] = std::cos(z);
        translation[12] = node.pos[0]; translation[13] = node.pos[1]; translation[14] = node.pos[2];
        float xy[16], rotation[16], scaled[16];
        Multiply(rx, ry, xy);
        Multiply(xy, rz, rotation);
        Multiply(scale, rotation, scaled);
        Multiply(scaled, translation, out);
    }
}

int main()
{
    hier::Node root = MakeNode(3.0f, -4.0f, 5.0f, -1);
    std::vector<hier::Node> nodes(1, root);
    std::vector<hier::Resolved> resolved;
    hier::Resolve(nodes, resolved);
    float legacy[16];
    LegacyCompose(root, legacy);
    if (!MatNear(resolved[0].world, legacy))
    {
        std::cerr << "depth-zero composition changed\n";
        return 1;
    }

    nodes.push_back(MakeNode(2.0f, 0.0f, 0.0f, 0));
    nodes[1].rot[0] = nodes[1].rot[1] = nodes[1].rot[2] = 0.0f;
    nodes[1].scale[0] = nodes[1].scale[1] = nodes[1].scale[2] = 1.0f;
    nodes.push_back(MakeNode(0.0f, 3.0f, 0.0f, 1));
    nodes[2].rot[0] = nodes[2].rot[1] = nodes[2].rot[2] = 0.0f;
    nodes[2].scale[0] = nodes[2].scale[1] = nodes[2].scale[2] = 1.0f;
    nodes[0].visible = false;
    hier::Resolve(nodes, resolved);
    float childLocal[16], expectedChild[16], grandLocal[16], expectedGrand[16];
    hier::Compose(nodes[1], childLocal);
    hier::Multiply(childLocal, resolved[0].world, expectedChild);
    hier::Compose(nodes[2], grandLocal);
    hier::Multiply(grandLocal, expectedChild, expectedGrand);
    if (!MatNear(resolved[1].world, expectedChild) || !MatNear(resolved[2].world, expectedGrand) ||
        resolved[0].visible || resolved[1].visible || resolved[2].visible)
    {
        std::cerr << "chain resolution or visibility failed\n";
        return 1;
    }

    std::vector<std::string> names;
    names.push_back("A"); names.push_back("B"); names.push_back("C"); names.push_back("D");
    std::vector<std::string> parentNames;
    parentNames.push_back("B"); parentNames.push_back("A"); parentNames.push_back("Missing"); parentNames.push_back("D");
    std::vector<int> parents;
    hier::MapParents(names, parentNames, parents);
    if (parents[0] != -1 || parents[1] != -1 || parents[2] != -1 || parents[3] != -1)
    {
        std::cerr << "invalid parent tolerance failed\n";
        return 1;
    }

    float inverseParent[16], recoveredLocal[16];
    hier::AffineInverse(resolved[0].world, inverseParent);
    hier::Multiply(resolved[1].world, inverseParent, recoveredLocal);
    float pos[3], rot[3], scale[3];
    hier::Decompose(recoveredLocal, pos, rot, scale);
    hier::Node roundTrip = nodes[1];
    for (int axis = 0; axis < 3; ++axis)
    {
        roundTrip.pos[axis] = pos[axis];
        roundTrip.rot[axis] = rot[axis];
        roundTrip.scale[axis] = scale[axis];
    }
    float recomposed[16];
    hier::Compose(roundTrip, recomposed);
    if (!MatNear(recoveredLocal, recomposed, 1.0e-3f))
    {
        std::cerr << "reparent decomposition round-trip failed\n";
        return 1;
    }
    return 0;
}