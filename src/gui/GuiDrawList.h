#pragma once

#include <vector>

// The render contract between the shared GUI core and the two D3D9 backends.
// The core never touches a device; it produces this and nothing else. Strict
// C++03 (compiled by the XDK toolset too).

namespace gui
{
    // What a backend does with a quad:
    //   Solid   — ignore the texture, paint the flat color
    //   Texture — modulate the color by the sampled RGBA
    //   Glyph   — read the SDF distance out of the atlas ALPHA and antialias it
    //             (same convention as the cooked Text attribute atlas)
    enum QuadKind { QuadSolid = 0, QuadTexture, QuadGlyph };

    // One screen-space quad in 1280x720 reference space, top-left origin.
    // Emitted already in painter's order (depth-first, parents before children,
    // earlier siblings behind later ones) — backends draw them as given and
    // never re-sort.
    struct Quad
    {
        float x0, y0, x1, y1;
        float u0, v0, u1, v1;
        float rgba[4];
        int   kind;      // QuadKind
        int   textureId; // host texture id; -1 when there is nothing to sample
        Quad()
            : x0(0.0f), y0(0.0f), x1(0.0f), y1(0.0f),
              u0(0.0f), v0(0.0f), u1(1.0f), v1(1.0f),
              kind(QuadSolid), textureId(-1)
        { rgba[0] = rgba[1] = rgba[2] = rgba[3] = 1.0f; }
    };

    // A run of consecutive quads a backend may submit in ONE draw call: same
    // kind, same texture, same color. A whole label collapses into one batch,
    // which is what keeps a text-heavy menu cheap on the console (where the
    // GUI pass replays once per tile band).
    struct Batch
    {
        int   first;
        int   count;
        int   kind;
        int   textureId;
        float rgba[4];
        Batch() : first(0), count(0), kind(QuadSolid), textureId(-1)
        { rgba[0] = rgba[1] = rgba[2] = rgba[3] = 1.0f; }
    };

    struct DrawList
    {
        std::vector<Quad>  quads;
        std::vector<Batch> batches;

        void Clear() { quads.clear(); batches.clear(); }
        bool Empty() const { return quads.empty(); }
    };
}
