#pragma once

// Shared scene hierarchy math for the editor and Xbox 360 runtime.
// Header-only and strict C++03 for the XDK VS2010 toolset.

#include <math.h>
#include <string>
#include <vector>

namespace hier
{
    struct Node
    {
        float pos[3];
        float rot[3];
        float scale[3];
        bool visible;
        int parent;
    };

    struct Resolved
    {
        float world[16];
        float pos[3];
        float rot[3];
        float scale[3];
        bool visible;
    };

    inline void Identity(float out[16])
    {
        for (int i = 0; i < 16; ++i) out[i] = 0.0f;
        out[0] = out[5] = out[10] = out[15] = 1.0f;
    }

    inline void Multiply(const float a[16], const float b[16], float out[16])
    {
        float result[16];
        for (int row = 0; row < 4; ++row)
            for (int col = 0; col < 4; ++col)
                result[row * 4 + col] =
                    a[row * 4 + 0] * b[0 * 4 + col] +
                    a[row * 4 + 1] * b[1 * 4 + col] +
                    a[row * 4 + 2] * b[2 * 4 + col] +
                    a[row * 4 + 3] * b[3 * 4 + col];
        for (int i = 0; i < 16; ++i) out[i] = result[i];
    }

    inline void Compose(const Node& node, float out[16])
    {
        const float d2r = 3.14159265f / 180.0f;
        const float x = node.rot[0] * d2r;
        const float y = node.rot[1] * d2r;
        const float z = node.rot[2] * d2r;
        const float cx = cosf(x), sx = sinf(x);
        const float cy = cosf(y), sy = sinf(y);
        const float cz = cosf(z), sz = sinf(z);
        float scale[16], rx[16], ry[16], rz[16], translation[16];
        Identity(scale); Identity(rx); Identity(ry); Identity(rz); Identity(translation);
        scale[0] = node.scale[0]; scale[5] = node.scale[1]; scale[10] = node.scale[2];
        rx[5] = cx; rx[6] = sx; rx[9] = -sx; rx[10] = cx;
        ry[0] = cy; ry[2] = -sy; ry[8] = sy; ry[10] = cy;
        rz[0] = cz; rz[1] = sz; rz[4] = -sz; rz[5] = cz;
        translation[12] = node.pos[0]; translation[13] = node.pos[1]; translation[14] = node.pos[2];

        float xy[16], rotation[16], scaled[16];
        Multiply(rx, ry, xy);
        Multiply(xy, rz, rotation);
        Multiply(scale, rotation, scaled);
        Multiply(scaled, translation, out);
    }

    inline void Decompose(const float m[16], float pos[3], float rotDeg[3], float scale[3])
    {
        scale[0] = sqrtf(m[0]*m[0] + m[1]*m[1] + m[2]*m[2]);
        scale[1] = sqrtf(m[4]*m[4] + m[5]*m[5] + m[6]*m[6]);
        scale[2] = sqrtf(m[8]*m[8] + m[9]*m[9] + m[10]*m[10]);

        float r[9];
        const float sx = scale[0] > 0.000001f ? scale[0] : 1.0f;
        const float sy = scale[1] > 0.000001f ? scale[1] : 1.0f;
        const float sz = scale[2] > 0.000001f ? scale[2] : 1.0f;
        r[0] = m[0] / sx; r[1] = m[1] / sx; r[2] = m[2] / sx;
        r[3] = m[4] / sy; r[4] = m[5] / sy; r[5] = m[6] / sy;
        r[6] = m[8] / sz; r[7] = m[9] / sz; r[8] = m[10] / sz;

        float sinY = -r[2];
        if (sinY < -1.0f) sinY = -1.0f;
        if (sinY > 1.0f) sinY = 1.0f;
        const float y = asinf(sinY);
        const float cy = cosf(y);
        float x, z;
        if (fabsf(cy) > 0.00001f)
        {
            x = atan2f(r[5], r[8]);
            z = atan2f(r[1], r[0]);
        }
        else
        {
            x = atan2f(-r[7], r[4]);
            z = 0.0f;
        }

        const float r2d = 180.0f / 3.14159265f;
        rotDeg[0] = x * r2d;
        rotDeg[1] = y * r2d;
        rotDeg[2] = z * r2d;
        pos[0] = m[12]; pos[1] = m[13]; pos[2] = m[14];
    }

    inline void AffineInverse(const float m[16], float out[16])
    {
        const float det =
            m[0] * (m[5] * m[10] - m[6] * m[9]) -
            m[1] * (m[4] * m[10] - m[6] * m[8]) +
            m[2] * (m[4] * m[9] - m[5] * m[8]);
        if (fabsf(det) <= 0.0000001f)
        {
            Identity(out);
            return;
        }

        const float invDet = 1.0f / det;
        Identity(out);
        out[0] =  (m[5] * m[10] - m[6] * m[9]) * invDet;
        out[1] =  (m[2] * m[9] - m[1] * m[10]) * invDet;
        out[2] =  (m[1] * m[6] - m[2] * m[5]) * invDet;
        out[4] =  (m[6] * m[8] - m[4] * m[10]) * invDet;
        out[5] =  (m[0] * m[10] - m[2] * m[8]) * invDet;
        out[6] =  (m[2] * m[4] - m[0] * m[6]) * invDet;
        out[8] =  (m[4] * m[9] - m[5] * m[8]) * invDet;
        out[9] =  (m[1] * m[8] - m[0] * m[9]) * invDet;
        out[10] = (m[0] * m[5] - m[1] * m[4]) * invDet;
        out[12] = -(m[12] * out[0] + m[13] * out[4] + m[14] * out[8]);
        out[13] = -(m[12] * out[1] + m[13] * out[5] + m[14] * out[9]);
        out[14] = -(m[12] * out[2] + m[13] * out[6] + m[14] * out[10]);
    }

    inline void MapParents(const std::vector<std::string>& names,
                           const std::vector<std::string>& parents,
                           std::vector<int>& out)
    {
        out.assign(names.size(), -1);
        for (size_t child = 0; child < names.size() && child < parents.size(); ++child)
        {
            if (parents[child].empty() || parents[child] == names[child]) continue;
            for (size_t candidate = 0; candidate < names.size(); ++candidate)
                if (candidate != child && names[candidate] == parents[child])
                {
                    out[child] = (int)candidate;
                    break;
                }
        }

        for (size_t start = 0; start < out.size(); ++start)
        {
            std::vector<int> path;
            int current = (int)start;
            while (current >= 0 && current < (int)out.size())
            {
                size_t seen = 0;
                for (; seen < path.size(); ++seen)
                    if (path[seen] == current) break;
                if (seen < path.size())
                {
                    for (size_t cycle = seen; cycle < path.size(); ++cycle)
                        out[path[cycle]] = -1;
                    break;
                }
                path.push_back(current);
                current = out[current];
            }
        }
    }

    inline bool IsDescendant(const std::vector<int>& parents, int candidate, int of)
    {
        if (candidate < 0 || candidate >= (int)parents.size()) return false;
        std::vector<bool> seen(parents.size(), false);
        int current = candidate;
        while (current >= 0 && current < (int)parents.size() && !seen[current])
        {
            if (current == of) return candidate != of;
            seen[current] = true;
            current = parents[current];
        }
        return false;
    }

    inline void ResolveOne(int index, const std::vector<Node>& nodes,
                           const std::vector<int>& parents, std::vector<int>& state,
                           std::vector<Resolved>& out)
    {
        if (state[index] == 2) return;
        state[index] = 1;
        const int parent = parents[index];
        float local[16];
        Compose(nodes[index], local);
        if (parent >= 0 && parent < (int)nodes.size() && state[parent] != 1)
        {
            ResolveOne(parent, nodes, parents, state, out);
            Multiply(local, out[parent].world, out[index].world);
            out[index].visible = nodes[index].visible && out[parent].visible;
        }
        else
        {
            for (int element = 0; element < 16; ++element) out[index].world[element] = local[element];
            out[index].visible = nodes[index].visible;
        }
        Decompose(out[index].world, out[index].pos, out[index].rot, out[index].scale);
        state[index] = 2;
    }

    inline void Resolve(const std::vector<Node>& nodes, std::vector<Resolved>& out)
    {
        std::vector<int> parents(nodes.size(), -1);
        for (size_t i = 0; i < nodes.size(); ++i)
            if (nodes[i].parent >= 0 && nodes[i].parent < (int)nodes.size() && nodes[i].parent != (int)i)
                parents[i] = nodes[i].parent;

        for (size_t start = 0; start < parents.size(); ++start)
        {
            std::vector<int> path;
            int current = (int)start;
            while (current >= 0)
            {
                size_t seen = 0;
                for (; seen < path.size(); ++seen)
                    if (path[seen] == current) break;
                if (seen < path.size())
                {
                    for (size_t cycle = seen; cycle < path.size(); ++cycle)
                        parents[path[cycle]] = -1;
                    break;
                }
                path.push_back(current);
                current = parents[current];
            }
        }

        out.resize(nodes.size());
        std::vector<int> state(nodes.size(), 0);
        for (size_t i = 0; i < nodes.size(); ++i)
            ResolveOne((int)i, nodes, parents, state, out);
    }
}