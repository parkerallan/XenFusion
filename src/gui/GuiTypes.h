#pragma once

// Shared value types for the Lua-scriptable GUI. Strict C++03 (compiled by the
// XDK toolset too) — no D3D, no editor types, no exceptions.
//
// Everything is expressed in the same 1280x720 reference space the Image/Text
// attribute overlays use, so a menu authored once lands identically in the
// editor viewport during Play and on the console. Origin is TOP-LEFT, +y down.

namespace gui
{
    const float kRefWidth  = 1280.0f;
    const float kRefHeight = 720.0f;

    // How deep the tree may nest. Layout and emission recurse, so this is a
    // hard guard against a script building an unbounded chain of parents.
    const int kMaxDepth = 32;

    enum WidgetKind { KindPanel = 0, KindLabel, KindImage, KindButton };

    // 9-point anchoring: the named point of the PARENT rect and the same named
    // point of the CHILD rect are made to coincide, then offset by (x, y). This
    // makes "centered", "pinned to the bottom-right" etc. fall out of one rule.
    enum Anchor
    {
        AnchorTopLeft = 0, AnchorTop,    AnchorTopRight,
        AnchorLeft,        AnchorCenter, AnchorRight,
        AnchorBottomLeft,  AnchorBottom, AnchorBottomRight,
        AnchorCount
    };

    enum AlignH { AlignLeft = 0, AlignCenterH, AlignRight };
    enum AlignV { AlignTop = 0, AlignMiddle, AlignBottom };

    enum NavDir { NavUp = 0, NavDown, NavLeft, NavRight, NavDirCount };

    struct Rect
    {
        float x0, y0, x1, y1;
        Rect() : x0(0.0f), y0(0.0f), x1(0.0f), y1(0.0f) {}
        float Width()   const { return x1 - x0; }
        float Height()  const { return y1 - y0; }
        float CenterX() const { return (x0 + x1) * 0.5f; }
        float CenterY() const { return (y0 + y1) * 0.5f; }
    };

    // A widget reference handed out to Lua. `generation` is what makes a stale
    // handle safe: when a widget is destroyed its slot's generation is bumped,
    // so an old handle resolves to nothing instead of silently aliasing the new
    // widget that later reused the slot.
    struct Handle
    {
        int          index;
        unsigned int generation;
        Handle() : index(-1), generation(0u) {}
        Handle(int i, unsigned int g) : index(i), generation(g) {}
        bool Valid() const { return index >= 0; }
    };

    // Anchor -> fraction along the rect (0 = left/top, 0.5 = middle, 1 = right/
    // bottom). The enum is laid out row-major 3x3, so this is pure arithmetic.
    inline float AnchorFracX(int anchor) { return (float)(anchor % 3) * 0.5f; }
    inline float AnchorFracY(int anchor) { return (float)(anchor / 3) * 0.5f; }
}
