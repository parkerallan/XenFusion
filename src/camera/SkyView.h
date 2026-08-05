#pragma once

// The skybox is a screen-space quad, so every pixel has to work out which way it
// is looking. That direction comes straight out of the view and projection
// matrices the pass is already using — nothing extra is plumbed through, and the
// same call site works for the editor orbit camera, the game Camera attribute at
// any FOV, and the six 90-degree faces of the reflection capture.
//
// Row-vector convention, matching camera/EnvCubeViews.h: columns 0/1/2 of the
// view matrix are the camera's world right / up / forward. The projection holds
// the lens — _11 = 1/(aspect*tan(fov/2)) and _22 = 1/tan(fov/2) — so dividing by
// them scales right/up to the half-extents of the near plane at unit depth:
//
//     dir = fwd + ndc.x * right + ndc.y * up
//
// which is exactly what skybox.hlsl interpolates across the quad. Header-only,
// C++03, no D3D types, so both targets compile it.
namespace sky
{
    // Degrees to turns. The shader adds this straight onto the u coordinate,
    // which spans one full revolution, so rotation never needs a matrix.
    const float kRotToTurns = 1.0f / 360.0f;

    // view16/proj16 are 16 floats in row-major order (D3DMATRIX layout).
    inline void Basis(const float* view16, const float* proj16,
                      float* right3, float* up3, float* fwd3)
    {
        // A degenerate projection would divide by zero; fall back to a 90-degree
        // lens rather than filling the screen with NaN.
        const float sx = (proj16[0]  != 0.0f) ? (1.0f / proj16[0])  : 1.0f;
        const float sy = (proj16[5]  != 0.0f) ? (1.0f / proj16[5])  : 1.0f;

        right3[0] = view16[0] * sx; right3[1] = view16[4] * sx; right3[2] = view16[8]  * sx;
        up3[0]    = view16[1] * sy; up3[1]    = view16[5] * sy; up3[2]    = view16[9]  * sy;
        fwd3[0]   = view16[2];      fwd3[1]   = view16[6];      fwd3[2]   = view16[10];
    }
}
