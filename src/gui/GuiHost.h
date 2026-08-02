#pragma once

namespace text { struct FontMetrics; }

namespace gui
{
    // The ONE per-target seam in the whole GUI subsystem. Unlike video/audio —
    // where the decoders genuinely differ and needed ScriptHost virtuals — the
    // GUI's tree, layout, focus and event logic is entirely shared code. Only
    // getting bytes onto the GPU differs:
    //
    //   editor  — paths resolve against the project root; textures load through
    //             the mesh texture cache and fonts bake an SDF atlas live from
    //             the source TTF
    //   console — paths resolve out of game.spak through the ASYNC StreamCache
    //
    // Every accessor may answer "not ready". That is not an error case to work
    // around: the console streams, so a menu's first frames can legitimately
    // have no atlas resident yet. Widgets whose asset is not ready are skipped
    // for that frame rather than drawn wrong.
    struct HostAssets
    {
        virtual ~HostAssets() {}

        // Opaque backend texture id for a project-relative path, or -1 while
        // unavailable. Ids must stay stable for the lifetime of the context.
        virtual int AcquireTexture(const char* relPath) = 0;

        // Pixel dimensions behind a texture id, used to place 9-slice borders.
        // Returns false for an unknown id.
        virtual bool TextureSize(int textureId, int& outWidth, int& outHeight) = 0;

        // Glyph metrics for a font, or NULL while unavailable. The pointer must
        // stay valid until the next Clear() — the core reads it immediately.
        virtual const text::FontMetrics* AcquireFont(const char* relPath) = 0;

        // Texture id of that font's SDF glyph atlas (-1 while unavailable).
        virtual int AcquireFontAtlas(const char* relPath) = 0;
    };
}
