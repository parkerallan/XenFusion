#pragma once

#include <string>
#include <vector>

#include "gui/GuiTypes.h"
#include "gui/GuiDrawList.h"

namespace input { struct InputState; }

namespace gui
{
    struct HostAssets;

    // The retained GUI tree: the whole Lua-scriptable menu system's brain.
    // Shared C++03, compiled by the editor and the XDK toolset alike; it owns
    // no device resources and calls no platform API. Scripts build a tree once
    // and keep handles to it, so a steady-state frame costs a layout check and
    // a draw-list emit — no per-frame Lua churn.
    //
    // Widgets live in a flat vector with a free list. Index 0 is an implicit
    // root covering the full 1280x720 reference screen, so parent-relative
    // anchoring needs no special case for top-level widgets.
    class Context
    {
    public:
        Context();
        ~Context();

        // Bind the asset seam and drop any previous tree. Call when a Play
        // session starts; Clear() when it stops.
        void Begin(HostAssets* assets);
        void Clear();
        bool Empty() const;

        Handle Root() const;

        // --- tree ---------------------------------------------------------
        // parent may be an invalid handle, meaning "child of the root".
        // Returns an invalid handle if the tree is full or too deep.
        Handle Create(int kind, Handle parent);
        void   Destroy(Handle h);       // destroys the subtree
        bool   Alive(Handle h) const;

        // --- properties (every setter is a no-op on a stale handle) --------
        void SetAnchor(Handle h, int anchor);
        void SetPos(Handle h, float x, float y);
        void SetSize(Handle h, float w, float height);
        void SetColor(Handle h, float r, float g, float b, float a);
        void SetFocusColor(Handle h, float r, float g, float b, float a);
        void SetTextColor(Handle h, float r, float g, float b, float a);
        void SetFocusTextColor(Handle h, float r, float g, float b, float a);
        void SetVisible(Handle h, bool visible);
        void SetEnabled(Handle h, bool enabled);
        void SetTexture(Handle h, const char* relPath);
        void SetSlice(Handle h, float slice);
        void SetText(Handle h, const char* value);
        void SetFont(Handle h, const char* relPath);
        void SetFontSize(Handle h, float pixels);
        void SetAlign(Handle h, int alignH, int alignV);
        void SetWrap(Handle h, bool wrap);
        void SetFocusable(Handle h, bool focusable);

        // --- queries ------------------------------------------------------
        bool Visible(Handle h) const;
        bool Enabled(Handle h) const;
        Rect SolvedRect(Handle h);              // solves the layout if dirty
        const char* Text(Handle h) const;       // "" on a stale handle

        // --- focus --------------------------------------------------------
        // A widget is reachable by navigation when it is focusable, enabled and
        // visible (itself and every ancestor). Phase 2 drives this from input.
        void   SetFocus(Handle h);
        Handle Focus() const;

        // --- pause --------------------------------------------------------
        // Menus need gameplay frozen. The core only carries the flag; each host
        // reads it to skip its physics step / animation / audio while keeping
        // rendering and script on_update running.
        void SetPaused(bool paused) { m_paused = paused; }
        bool Paused() const         { return m_paused; }

        // --- per-frame ----------------------------------------------------
        void Update(float dt, const input::InputState& in);
        const DrawList& Emit();

        // Events raised by Update, drained by the script binding into Lua
        // callbacks. FIFO; PopEvent returns false when the queue is empty.
        enum EventType { EvConfirm = 0, EvCancel, EvFocus };
        struct Event
        {
            int    type;
            Handle widget;
            Event() : type(EvConfirm) {}
        };
        bool PopEvent(Event& out);

    private:
        struct Widget
        {
            unsigned int generation;
            bool         alive;

            int              kind;
            int              parent;
            std::vector<int> children;

            int   anchor;
            float x, y, w, h;
            bool  visible;
            bool  enabled;

            // `color` is the widget's own colour: a Label's glyphs, everything
            // else's background. Widgets that have BOTH (Button, a titled
            // Panel) draw their glyphs in textColor instead — otherwise a
            // focused button's caption would vanish into its own highlight.
            float       color[4];
            float       focusColor[4];
            float       textColor[4];
            float       focusTextColor[4];
            std::string texturePath;
            float       slice;      // 9-slice border, in reference pixels

            std::string text;
            std::string fontPath;
            float       fontSize;
            int         alignH, alignV;
            bool        wrap;

            bool  focusable;
            Rect  rect;             // solved, absolute

            Widget();
            void Reset();
        };

        // Resolve a handle to a live widget index, or -1.
        int  Resolve(Handle h) const;
        int  ResolveMutable(Handle h);   // also marks the layout dirty

        void Solve();
        void SolveChildren(int index, int depth);
        int  Depth(int index) const;

        void EmitWidget(int index);
        void EmitBackground(const Widget& w, const float* color);
        void EmitNineSlice(const Widget& w, const float* color,
                           int textureId, int texW, int texH);
        void EmitText(const Widget& w, const float* color);
        void BuildBatches();

        void DestroyRecursive(int index);
        void DetachFromParent(int index);
        bool EffectivelyVisible(int index) const;

        // Reachable by navigation: focusable, enabled, and visible all the way
        // up to the root.
        bool Navigable(int index) const;
        int  FirstNavigable() const;
        int  PickInDirection(int from, int dir) const;
        void MoveFocus(int dir);
        void RaiseEvent(int type, int widgetIndex);

        HostAssets*         m_assets;
        std::vector<Widget> m_widgets;
        std::vector<int>    m_free;
        DrawList            m_draw;
        std::vector<Event>  m_events;

        int  m_focus;            // widget index, -1 = none
        bool m_layoutDirty;
        bool m_paused;

        // Directional-repeat state: a held direction fires once, waits out an
        // initial delay, then repeats. Owning this here is the point — no menu
        // script should ever have to write a repeat loop.
        int   m_navHeld;         // NavDir currently held, -1 = none
        float m_navTimer;        // seconds until the next repeat fires
        bool  m_prevConfirm;
        bool  m_prevCancel;

        Context(const Context&);              // non-copyable
        Context& operator=(const Context&);
    };
}
