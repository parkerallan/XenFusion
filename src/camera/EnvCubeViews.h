#pragma once

// Reflections photograph the scene from the reflecting object's position: six
// renders, one per axis, stitched into a cube map that standard.hlsl samples on
// s5. This header has the six cameras that take those renders, plus the
// settings for the blur chain built over the result (roughness reads a level of
// it). Both are here because the editor and the 360 runtime each run this
// capture and must produce identical cubes. Header-only, C++03, no D3D types.
// Faces are in D3DCUBEMAP_FACE_POSITIVE_X .. NEGATIVE_Z order.
namespace envcube
{
    // kMaxMip is uploaded to standard.hlsl c22.x. Stopping at 32 is deliberate:
    // blur comes from BlurPasses, not from picking a coarser level.
    const int   kSize   = 128;
    const int   kMips   = 3;     // 128 / 64 / 32
    const float kMaxMip = 2.0f;

    // Kernel tap spacing in face-space units (a face spans [-1,1]). TapStep is
    // for the pass that halves resolution, TapStepAt for one filtering a level
    // in place. Kept at ~1.25 texels: wider leaves gaps between taps, and gaps
    // turn bright features into rings. Level 0 gets 0, so it stays an untouched
    // copy of the capture for mirror-smooth surfaces.
    inline float TapStep(int level)
    {
        if (level <= 0) return 0.0f;
        return (2.0f / (float)(kSize >> (level - 1))) * 1.25f;
    }

    inline float TapStepAt(int level)
    {
        return (2.0f / (float)(kSize >> level)) * 1.25f;
    }

    // One pass halves resolution, the rest repeat at that resolution and are
    // what make a level blurry rather than just small. Blur adds up as
    // sqrt(passes). Never trade passes for a coarser level — magnified, a
    // coarse grid shows as a quilt of hard-edged patches.
    inline int BlurPasses(int level)
    {
        if (level <= 0) return 1;
        return (level == kMips - 1) ? 10 : 4;
    }

    // Stretches a face so its outer texel centres sit on the shared edges.
    // D3D9/Xenos cannot filter across a face boundary, so without this adjacent
    // faces disagree at the seam and a ridge shows. Level 0 is left exact.
    inline float EdgeFixup(int level)
    {
        if (level <= 0) return 1.0f;
        const float dst = (float)(kSize >> level);
        return dst / (dst - 1.0f);
    }

    // Axes FaceView is built from. env_blur.hlsl runs the mapping backwards
    // with them: texel (u,v) of face f holds fwd + (2u-1)*right + (1-2v)*up.
    inline void FaceBasis(int f, float* r3, float* u3, float* f3)
    {
        static const float R[6][3] = { {0,0,-1}, {0,0,1},  {1,0,0},  {1,0,0}, {1,0,0},  {-1,0,0} };
        static const float U[6][3] = { {0,1,0},  {0,1,0},  {0,0,-1}, {0,0,1}, {0,1,0},  {0,1,0} };
        static const float F[6][3] = { {1,0,0},  {-1,0,0}, {0,1,0},  {0,-1,0},{0,0,1},  {0,0,-1} };
        for (int i = 0; i < 3; ++i) { r3[i] = R[f][i]; u3[i] = U[f][i]; f3[i] = F[f][i]; }
    }

    // Camera for face f standing at p; 4x4 row-vector view matrix into `out`.
    inline void FaceView(int f, const float* p, float* out)
    {
        float R3[3], U3[3], F3[3];
        FaceBasis(f, R3, U3, F3);
        const float* r = R3; const float* u = U3; const float* w = F3;
        out[0]  = r[0]; out[1]  = u[0]; out[2]  = w[0]; out[3]  = 0.0f;
        out[4]  = r[1]; out[5]  = u[1]; out[6]  = w[1]; out[7]  = 0.0f;
        out[8]  = r[2]; out[9]  = u[2]; out[10] = w[2]; out[11] = 0.0f;
        out[12] = -(p[0]*r[0] + p[1]*r[1] + p[2]*r[2]);
        out[13] = -(p[0]*u[0] + p[1]*u[1] + p[2]*u[2]);
        out[14] = -(p[0]*w[0] + p[1]*w[1] + p[2]*w[2]);
        out[15] = 1.0f;
    }

    // Shared lens: 90-degree square, so the six frustums tile every direction
    // exactly once. Row-vector, 16 floats.
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
