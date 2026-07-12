#pragma once

// Minimal left-handed, row-major matrix helpers (matching D3D9 row-vector
// convention) — the same hand-rolled math the PC editor's SceneRenderer uses,
// kept identical so the runtime frames the scene the way the editor does. We
// roll our own rather than pull in d3dx9 math so world composition ordering is
// exactly the editor's (Scale * Rot(x,y,z) * Translate).

#include <xtl.h>
#include <math.h>

namespace rmath
{
    struct Vec3 { float x, y, z; };
    struct Vec4 { float x, y, z, w; };

    // Transform a point (row vector, w=1) by a matrix, keeping w (for clip-space
    // diagnostics: NDC = xyz / w).
    inline Vec4 TransformVec4(const Vec3& p, const D3DMATRIX& m)
    {
        Vec4 r;
        r.x = p.x * m._11 + p.y * m._21 + p.z * m._31 + m._41;
        r.y = p.x * m._12 + p.y * m._22 + p.z * m._32 + m._42;
        r.z = p.x * m._13 + p.y * m._23 + p.z * m._33 + m._43;
        r.w = p.x * m._14 + p.y * m._24 + p.z * m._34 + m._44;
        return r;
    }

    inline Vec3 Sub(const Vec3& a, const Vec3& b) { Vec3 r = { a.x - b.x, a.y - b.y, a.z - b.z }; return r; }
    inline float Dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
    inline Vec3 Cross(const Vec3& a, const Vec3& b)
    {
        Vec3 r = { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
        return r;
    }
    inline Vec3 Normalize(const Vec3& v)
    {
        float len = sqrtf(Dot(v, v));
        if (len < 1e-6f) { Vec3 z = { 0, 0, 0 }; return z; }
        Vec3 r = { v.x / len, v.y / len, v.z / len };
        return r;
    }

    inline D3DMATRIX Identity()
    {
        D3DMATRIX m;
        ZeroMemory(&m, sizeof(m));
        m._11 = m._22 = m._33 = m._44 = 1.0f;
        return m;
    }

    inline D3DMATRIX Multiply(const D3DMATRIX& a, const D3DMATRIX& b)
    {
        D3DMATRIX r;
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j)
                r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] +
                            a.m[i][2] * b.m[2][j] + a.m[i][3] * b.m[3][j];
        return r;
    }

    inline D3DMATRIX Translation(float x, float y, float z)
    {
        D3DMATRIX m = Identity();
        m._41 = x; m._42 = y; m._43 = z;
        return m;
    }

    inline D3DMATRIX Scaling(float x, float y, float z)
    {
        D3DMATRIX m = Identity();
        m._11 = x; m._22 = y; m._33 = z;
        return m;
    }

    inline D3DMATRIX RotationX(float a)
    {
        D3DMATRIX m = Identity();
        const float c = cosf(a), s = sinf(a);
        m._22 = c; m._23 = s; m._32 = -s; m._33 = c;
        return m;
    }
    inline D3DMATRIX RotationY(float a)
    {
        D3DMATRIX m = Identity();
        const float c = cosf(a), s = sinf(a);
        m._11 = c; m._13 = -s; m._31 = s; m._33 = c;
        return m;
    }
    inline D3DMATRIX RotationZ(float a)
    {
        D3DMATRIX m = Identity();
        const float c = cosf(a), s = sinf(a);
        m._11 = c; m._12 = s; m._21 = -s; m._22 = c;
        return m;
    }

    inline D3DMATRIX LookAtLH(const Vec3& eye, const Vec3& at, const Vec3& up)
    {
        Vec3 z = Normalize(Sub(at, eye));
        Vec3 x = Normalize(Cross(up, z));
        Vec3 y = Cross(z, x);

        D3DMATRIX m;
        ZeroMemory(&m, sizeof(m));
        m._11 = x.x; m._12 = y.x; m._13 = z.x; m._14 = 0.0f;
        m._21 = x.y; m._22 = y.y; m._23 = z.y; m._24 = 0.0f;
        m._31 = x.z; m._32 = y.z; m._33 = z.z; m._34 = 0.0f;
        m._41 = -Dot(x, eye); m._42 = -Dot(y, eye); m._43 = -Dot(z, eye); m._44 = 1.0f;
        return m;
    }

    inline D3DMATRIX PerspectiveFovLH(float fovY, float aspect, float zn, float zf)
    {
        float yScale = 1.0f / tanf(fovY * 0.5f);
        float xScale = yScale / aspect;

        D3DMATRIX m;
        ZeroMemory(&m, sizeof(m));
        m._11 = xScale;
        m._22 = yScale;
        m._33 = zf / (zf - zn);
        m._34 = 1.0f;
        m._43 = -zn * zf / (zf - zn);
        return m;
    }

    // Full 4x4 transpose. The 360's D3DXCompileShaderFromFile packs matrices
    // column-major (it ignores the ROWMAJOR flag the PC compiler honors), so the
    // runtime transposes its row-major matrices before SetVertexShaderConstantF —
    // the canonical XDK pattern (see the Dolphin sample).
    inline D3DMATRIX Transpose(const D3DMATRIX& m)
    {
        D3DMATRIX r;
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j)
                r.m[i][j] = m.m[j][i];
        return r;
    }

    // Transpose the rotation 3x3 (translation zeroed) — the inverse of a rotation.
    inline D3DMATRIX Transpose3(const D3DMATRIX& m)
    {
        D3DMATRIX r = Identity();
        r._11 = m._11; r._12 = m._21; r._13 = m._31;
        r._21 = m._12; r._22 = m._22; r._23 = m._32;
        r._31 = m._13; r._32 = m._23; r._33 = m._33;
        return r;
    }

    // Transform a point (row vector, w = 1) by a matrix.
    inline Vec3 TransformPoint(const Vec3& p, const D3DMATRIX& m)
    {
        Vec3 r = { p.x * m._11 + p.y * m._21 + p.z * m._31 + m._41,
                   p.x * m._12 + p.y * m._22 + p.z * m._32 + m._42,
                   p.x * m._13 + p.y * m._23 + p.z * m._33 + m._43 };
        return r;
    }

    // World = Scale * Rot(x,y,z) * Translation (degrees in), matching the editor.
    inline D3DMATRIX ComposeWorld(const float pos[3], const float rotDeg[3], const float scale[3])
    {
        const float d2r = 3.14159265f / 180.0f;
        const D3DMATRIX rot = Multiply(Multiply(RotationX(rotDeg[0] * d2r),
                                                RotationY(rotDeg[1] * d2r)),
                                       RotationZ(rotDeg[2] * d2r));
        return Multiply(Multiply(Scaling(scale[0], scale[1], scale[2]), rot),
                        Translation(pos[0], pos[1], pos[2]));
    }
}
