#pragma once

// Row-vector view/projection builders for rendering the scene into a cube
// map's six faces (D3DCUBEMAP_FACE_POSITIVE_X .. NEGATIVE_Z order), used by
// the dynamic environment capture that feeds metal reflections (standard.hlsl
// s5). Shared by the editor and the 360 runtime like CameraResolve.h —
// header-only, C++03, no D3D types (both sides memcpy into their D3DMATRIX).
namespace envcube
{
    // 4x4 row-vector view matrix for face f (0..5) at world position p.
    inline void FaceView(int f, const float* p, float* out)
    {
        static const float R[6][3] = { {0,0,-1}, {0,0,1},  {1,0,0},  {1,0,0}, {1,0,0},  {-1,0,0} };
        static const float U[6][3] = { {0,1,0},  {0,1,0},  {0,0,-1}, {0,0,1}, {0,1,0},  {0,1,0} };
        static const float F[6][3] = { {1,0,0},  {-1,0,0}, {0,1,0},  {0,-1,0},{0,0,1},  {0,0,-1} };
        const float* r = R[f]; const float* u = U[f]; const float* w = F[f];
        out[0]  = r[0]; out[1]  = u[0]; out[2]  = w[0]; out[3]  = 0.0f;
        out[4]  = r[1]; out[5]  = u[1]; out[6]  = w[1]; out[7]  = 0.0f;
        out[8]  = r[2]; out[9]  = u[2]; out[10] = w[2]; out[11] = 0.0f;
        out[12] = -(p[0]*r[0] + p[1]*r[1] + p[2]*r[2]);
        out[13] = -(p[0]*u[0] + p[1]*u[1] + p[2]*u[2]);
        out[14] = -(p[0]*w[0] + p[1]*w[1] + p[2]*w[2]);
        out[15] = 1.0f;
    }

    // 90-degree square projection (row-vector) — each face sees one quadrant.
    inline void FaceProj(float zn, float zf, float* out)
    {
        for (int i = 0; i < 16; ++i) out[i] = 0.0f;
        out[0]  = 1.0f;
        out[5]  = 1.0f;
        out[10] = zf / (zf - zn);
        out[11] = 1.0f;
        out[14] = -zn * zf / (zf - zn);
    }
}
