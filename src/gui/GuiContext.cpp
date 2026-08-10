#include "gui/GuiContext.h"
#include "gui/GuiHost.h"
#include "input/InputState.h"
#include "text/TextLayout.h"

// Shared GUI core. Strict C++03 (compiled by the XDK toolset too): no auto, no
// range-for, no nullptr, no <algorithm> beyond what the other shared modules
// already use.

namespace
{
    const int kRootIndex = 0;

    // A tree big enough for any menu, small enough that a runaway script that
    // creates widgets in on_update fails loudly instead of eating the console's
    // memory. Slots are recycled, so this bounds LIVE widgets, not total churn.
    const int kMaxWidgets = 4096;

    // Directional repeat. Tuned to feel like a console system menu: the first
    // step is instant, then a beat before it starts scrolling.
    const float kNavInitialDelay = 0.36f;
    const float kNavRepeatDelay  = 0.11f;
    // How far the stick must travel before it counts as a direction. High
    // enough that a diagonal push does not fire both axes.
    const float kStickDeadzone   = 0.55f;

    void CopyColor(float* dst, const float* src)
    {
        dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2]; dst[3] = src[3];
    }
    bool SameColor(const float* a, const float* b)
    {
        return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3];
    }
}

namespace gui
{
    // generation starts at 1 so a default-constructed Handle (index -1,
    // generation 0) can never accidentally match a live slot.
    Context::Widget::Widget() : generation(1u) { Reset(); }

    void Context::Widget::Reset()
    {
        // generation is deliberately NOT reset — it only ever counts up, which
        // is what makes a stale handle detectable after the slot is recycled.
        alive   = false;
        kind    = KindPanel;
        parent  = -1;
        children.clear();

        anchor  = AnchorTopLeft;
        x = y = 0.0f;
        w = 100.0f;
        h = 100.0f;
        visible = true;
        enabled = true;

        color[0] = color[1] = color[2] = color[3] = 1.0f;
        // Focused default: the same colour at full white. Scripts that care set
        // their own; scripts that don't still get a visible highlight because
        // the unfocused colour is what they dim.
        focusColor[0] = focusColor[1] = focusColor[2] = focusColor[3] = 1.0f;
        textColor[0] = textColor[1] = textColor[2] = textColor[3] = 1.0f;
        focusTextColor[0] = focusTextColor[1] = focusTextColor[2] = focusTextColor[3] = 1.0f;
        texturePath.clear();
        slice = 0.0f;
        playMode = gifanim::PlayLoop;
        gifPlay  = gifanim::Playback();

        text.clear();
        fontPath.clear();
        fontSize = 32.0f;
        alignH = AlignLeft;
        alignV = AlignTop;
        wrap   = false;

        focusable = false;
        rect = Rect();
    }

    Context::Context()
        : m_assets(0), m_focus(-1), m_layoutDirty(true), m_paused(false), m_dt(0.0f),
          m_navHeld(-1), m_navTimer(0.0f), m_prevConfirm(false), m_prevCancel(false)
    {
        Clear();
    }

    Context::~Context() {}

    void Context::Begin(HostAssets* assets)
    {
        Clear();
        m_assets = assets;
    }

    void Context::Clear()
    {
        m_widgets.clear();
        m_free.clear();
        m_draw.Clear();
        m_events.clear();
        m_focus = -1;
        m_layoutDirty = true;
        m_paused = false;
        m_navHeld = -1;
        m_navTimer = 0.0f;
        m_prevConfirm = false;
        m_prevCancel = false;

        // The implicit root: a full-screen, invisible container. It is never
        // drawn itself, so top-level widgets can anchor against the screen with
        // exactly the same code path as any nested child.
        Widget root;
        root.alive   = true;
        root.kind    = KindPanel;
        root.parent  = -1;
        root.color[3] = 0.0f;
        root.w = kRefWidth;
        root.h = kRefHeight;
        m_widgets.push_back(root);
    }

    bool Context::Empty() const
    {
        return m_widgets.size() <= 1 || m_widgets[kRootIndex].children.empty();
    }

    Handle Context::Root() const
    {
        return Handle(kRootIndex, m_widgets[kRootIndex].generation);
    }

    int Context::Resolve(Handle h) const
    {
        if (h.index < 0 || h.index >= (int)m_widgets.size())
            return -1;
        const Widget& w = m_widgets[h.index];
        if (!w.alive || w.generation != h.generation)
            return -1;
        return h.index;
    }

    int Context::ResolveMutable(Handle h)
    {
        const int index = Resolve(h);
        if (index >= 0)
            m_layoutDirty = true;
        return index;
    }

    bool Context::Alive(Handle h) const { return Resolve(h) >= 0; }

    int Context::Depth(int index) const
    {
        int depth = 0;
        int walk = index;
        while (walk > 0 && depth <= kMaxDepth)
        {
            walk = m_widgets[walk].parent;
            ++depth;
        }
        return depth;
    }

    Handle Context::Create(int kind, Handle parent)
    {
        int parentIndex = Resolve(parent);
        if (parentIndex < 0)
            parentIndex = kRootIndex;
        if (Depth(parentIndex) + 1 > kMaxDepth)
            return Handle();

        int index;
        if (!m_free.empty())
        {
            index = m_free.back();
            m_free.pop_back();
            m_widgets[index].Reset();
        }
        else
        {
            if ((int)m_widgets.size() >= kMaxWidgets)
                return Handle();
            m_widgets.push_back(Widget());
            index = (int)m_widgets.size() - 1;
        }

        Widget& w = m_widgets[index];
        w.alive  = true;
        w.kind   = kind;
        w.parent = parentIndex;
        if (kind == KindLabel || kind == KindButton)
        {
            // Text widgets almost always want their string centred in the box;
            // making that the default keeps the common menu case declarative.
            w.alignH = AlignCenterH;
            w.alignV = AlignMiddle;
        }
        if (kind == KindButton)
            w.focusable = true;

        m_widgets[parentIndex].children.push_back(index);
        m_layoutDirty = true;
        return Handle(index, w.generation);
    }

    void Context::DetachFromParent(int index)
    {
        const int parent = m_widgets[index].parent;
        if (parent < 0 || parent >= (int)m_widgets.size())
            return;
        std::vector<int>& siblings = m_widgets[parent].children;
        for (size_t i = 0; i < siblings.size(); ++i)
        {
            if (siblings[i] != index)
                continue;
            siblings.erase(siblings.begin() + i);
            return;
        }
    }

    void Context::DestroyRecursive(int index)
    {
        // Copy the child list: the recursion mutates the vector we walk.
        std::vector<int> children = m_widgets[index].children;
        for (size_t i = 0; i < children.size(); ++i)
            DestroyRecursive(children[i]);

        Widget& w = m_widgets[index];
        w.children.clear();
        w.alive = false;
        w.parent = -1;
        ++w.generation;          // invalidates every handle Lua still holds
        w.texturePath.clear();
        w.text.clear();
        w.fontPath.clear();
        if (m_focus == index)
            m_focus = -1;
        m_free.push_back(index);
    }

    void Context::Destroy(Handle h)
    {
        const int index = Resolve(h);
        if (index <= kRootIndex)   // the root is not destroyable
            return;
        DetachFromParent(index);
        DestroyRecursive(index);
        m_layoutDirty = true;
    }

    // --- properties -------------------------------------------------------

    void Context::SetAnchor(Handle h, int anchor)
    {
        const int i = ResolveMutable(h);
        if (i < 0 || anchor < 0 || anchor >= AnchorCount) return;
        m_widgets[i].anchor = anchor;
    }
    void Context::SetPos(Handle h, float x, float y)
    {
        const int i = ResolveMutable(h);
        if (i < 0) return;
        m_widgets[i].x = x; m_widgets[i].y = y;
    }
    void Context::SetSize(Handle h, float w, float height)
    {
        const int i = ResolveMutable(h);
        if (i < 0) return;
        m_widgets[i].w = w; m_widgets[i].h = height;
    }
    void Context::SetColor(Handle h, float r, float g, float b, float a)
    {
        const int i = Resolve(h);
        if (i < 0) return;
        Widget& w = m_widgets[i];
        w.color[0] = r; w.color[1] = g; w.color[2] = b; w.color[3] = a;
    }
    void Context::SetFocusColor(Handle h, float r, float g, float b, float a)
    {
        const int i = Resolve(h);
        if (i < 0) return;
        Widget& w = m_widgets[i];
        w.focusColor[0] = r; w.focusColor[1] = g; w.focusColor[2] = b; w.focusColor[3] = a;
    }
    void Context::SetTextColor(Handle h, float r, float g, float b, float a)
    {
        const int i = Resolve(h);
        if (i < 0) return;
        Widget& w = m_widgets[i];
        w.textColor[0] = r; w.textColor[1] = g; w.textColor[2] = b; w.textColor[3] = a;
    }
    void Context::SetFocusTextColor(Handle h, float r, float g, float b, float a)
    {
        const int i = Resolve(h);
        if (i < 0) return;
        Widget& w = m_widgets[i];
        w.focusTextColor[0] = r; w.focusTextColor[1] = g;
        w.focusTextColor[2] = b; w.focusTextColor[3] = a;
    }
    void Context::SetVisible(Handle h, bool visible)
    {
        const int i = Resolve(h);
        if (i < 0) return;
        m_widgets[i].visible = visible;
    }
    void Context::SetEnabled(Handle h, bool enabled)
    {
        const int i = Resolve(h);
        if (i < 0) return;
        m_widgets[i].enabled = enabled;
    }
    void Context::SetTexture(Handle h, const char* relPath)
    {
        const int i = Resolve(h);
        if (i < 0) return;
        m_widgets[i].texturePath = relPath ? relPath : "";
    }
    void Context::SetSlice(Handle h, float slice)
    {
        const int i = Resolve(h);
        if (i < 0) return;
        m_widgets[i].slice = slice < 0.0f ? 0.0f : slice;
    }
    void Context::SetPlayMode(Handle h, int mode)
    {
        const int i = Resolve(h);
        if (i < 0) return;
        if (mode < gifanim::PlayOff || mode > gifanim::PlayLoop) mode = gifanim::PlayLoop;
        // gifanim::Advance rewinds on its own when it sees the mode change, so
        // this is only the assignment.
        m_widgets[i].playMode = mode;
    }
    void Context::SetText(Handle h, const char* value)
    {
        const int i = Resolve(h);
        if (i < 0) return;
        m_widgets[i].text = value ? value : "";
    }
    void Context::SetFont(Handle h, const char* relPath)
    {
        const int i = Resolve(h);
        if (i < 0) return;
        m_widgets[i].fontPath = relPath ? relPath : "";
    }
    void Context::SetFontSize(Handle h, float pixels)
    {
        const int i = Resolve(h);
        if (i < 0) return;
        m_widgets[i].fontSize = pixels > 0.0f ? pixels : 1.0f;
    }
    void Context::SetAlign(Handle h, int alignH, int alignV)
    {
        const int i = Resolve(h);
        if (i < 0) return;
        m_widgets[i].alignH = alignH;
        m_widgets[i].alignV = alignV;
    }
    void Context::SetWrap(Handle h, bool wrap)
    {
        const int i = Resolve(h);
        if (i < 0) return;
        m_widgets[i].wrap = wrap;
    }
    void Context::SetFocusable(Handle h, bool focusable)
    {
        const int i = Resolve(h);
        if (i < 0) return;
        m_widgets[i].focusable = focusable;
        if (!focusable && m_focus == i)
            m_focus = -1;
    }

    // --- queries ----------------------------------------------------------

    bool Context::Visible(Handle h) const
    {
        const int i = Resolve(h);
        return i >= 0 && m_widgets[i].visible;
    }
    bool Context::Enabled(Handle h) const
    {
        const int i = Resolve(h);
        return i >= 0 && m_widgets[i].enabled;
    }
    Rect Context::SolvedRect(Handle h)
    {
        const int i = Resolve(h);
        if (i < 0) return Rect();
        Solve();
        return m_widgets[i].rect;
    }
    const char* Context::Text(Handle h) const
    {
        const int i = Resolve(h);
        return i < 0 ? "" : m_widgets[i].text.c_str();
    }

    void Context::RaiseEvent(int type, int widgetIndex)
    {
        if (widgetIndex < 0 || widgetIndex >= (int)m_widgets.size())
            return;
        Event ev;
        ev.type = type;
        ev.widget = Handle(widgetIndex, m_widgets[widgetIndex].generation);
        m_events.push_back(ev);
    }

    void Context::SetFocus(Handle h)
    {
        const int i = Resolve(h);
        if (i < 0) { m_focus = -1; return; }
        if (!m_widgets[i].focusable)
            return;
        if (m_focus == i)
            return;
        m_focus = i;
        RaiseEvent(EvFocus, i);
    }

    Handle Context::Focus() const
    {
        if (m_focus < 0) return Handle();
        return Handle(m_focus, m_widgets[m_focus].generation);
    }

    bool Context::PopEvent(Event& out)
    {
        if (m_events.empty())
            return false;
        out = m_events[0];
        m_events.erase(m_events.begin());
        return true;
    }

    bool Context::EffectivelyVisible(int index) const
    {
        int walk = index;
        int guard = 0;
        while (walk >= 0 && guard++ <= kMaxDepth)
        {
            const Widget& w = m_widgets[walk];
            if (!w.alive || !w.visible)
                return false;
            walk = w.parent;
        }
        return true;
    }

    // --- layout -----------------------------------------------------------

    void Context::Solve()
    {
        if (!m_layoutDirty)
            return;
        Widget& root = m_widgets[kRootIndex];
        root.rect.x0 = 0.0f;
        root.rect.y0 = 0.0f;
        root.rect.x1 = kRefWidth;
        root.rect.y1 = kRefHeight;
        SolveChildren(kRootIndex, 0);
        m_layoutDirty = false;
    }

    void Context::SolveChildren(int index, int depth)
    {
        if (depth > kMaxDepth)
            return;
        const Rect parent = m_widgets[index].rect;
        const float parentW = parent.Width();
        const float parentH = parent.Height();

        // Copy: nothing mutates the tree here, but the child vector may be
        // reallocated by a nested push in future edits — cheap insurance.
        const std::vector<int> children = m_widgets[index].children;
        for (size_t i = 0; i < children.size(); ++i)
        {
            const int ci = children[i];
            Widget& w = m_widgets[ci];
            if (!w.alive)
                continue;

            // The anchor's point on the parent and the SAME point on the child
            // are made to coincide, then displaced by the authored offset.
            const float ax = AnchorFracX(w.anchor);
            const float ay = AnchorFracY(w.anchor);
            const float px = parent.x0 + ax * parentW;
            const float py = parent.y0 + ay * parentH;
            w.rect.x0 = px - ax * w.w + w.x;
            w.rect.y0 = py - ay * w.h + w.y;
            w.rect.x1 = w.rect.x0 + w.w;
            w.rect.y1 = w.rect.y0 + w.h;

            SolveChildren(ci, depth + 1);
        }
    }

    // --- per-frame --------------------------------------------------------

    // --- focus navigation -------------------------------------------------

    bool Context::Navigable(int index) const
    {
        if (index < 0 || index >= (int)m_widgets.size())
            return false;
        const Widget& w = m_widgets[index];
        return w.alive && w.focusable && w.enabled && EffectivelyVisible(index);
    }

    int Context::FirstNavigable() const
    {
        for (size_t i = 1; i < m_widgets.size(); ++i)
            if (Navigable((int)i))
                return (int)i;
        return -1;
    }

    // Pick the nearest navigable widget that actually lies in `dir`. Candidates
    // must clear the current rect along the travel axis, which is what stops
    // "down" from selecting something merely beside the current item. Cross-axis
    // drift is weighted heavily so a tidy column steps straight down even when a
    // stray widget sits closer as the crow flies.
    int Context::PickInDirection(int from, int dir) const
    {
        const Rect& src = m_widgets[from].rect;
        const float sx = src.CenterX();
        const float sy = src.CenterY();

        int best = -1;
        float bestScore = 0.0f;
        for (size_t i = 1; i < m_widgets.size(); ++i)
        {
            const int index = (int)i;
            if (index == from || !Navigable(index))
                continue;
            const Rect& dst = m_widgets[index].rect;
            const float dx = dst.CenterX() - sx;
            const float dy = dst.CenterY() - sy;

            float along = 0.0f, across = 0.0f;
            if (dir == NavUp)         { along = -dy; across = dx < 0.0f ? -dx : dx; }
            else if (dir == NavDown)  { along =  dy; across = dx < 0.0f ? -dx : dx; }
            else if (dir == NavLeft)  { along = -dx; across = dy < 0.0f ? -dy : dy; }
            else                      { along =  dx; across = dy < 0.0f ? -dy : dy; }

            if (along <= 0.0f)
                continue;   // not in this direction at all

            const float score = along + across * 3.0f;
            if (best < 0 || score < bestScore)
            {
                best = index;
                bestScore = score;
            }
        }
        return best;
    }

    void Context::MoveFocus(int dir)
    {
        if (m_focus < 0 || !Navigable(m_focus))
        {
            // No focus yet: any direction adopts the first navigable widget, so
            // a menu the script never called set_focus on is still usable.
            const int first = FirstNavigable();
            if (first >= 0)
            {
                m_focus = first;
                RaiseEvent(EvFocus, first);
            }
            return;
        }
        const int next = PickInDirection(m_focus, dir);
        if (next < 0 || next == m_focus)
            return;         // edge of the menu: stay put rather than wrap
        m_focus = next;
        RaiseEvent(EvFocus, next);
    }

    void Context::Update(float dt, const input::InputState& in)
    {
        // Emit() steps GIF widgets and is the only place the host — and so a
        // GIF's frame count — is reachable, but it takes no arguments.
        m_dt = dt;
        Solve();

        // A widget can be hidden, disabled or destroyed by the very callback
        // that ran last frame — drop focus rather than navigate from a rect
        // that is no longer on screen.
        if (m_focus >= 0 && !Navigable(m_focus))
            m_focus = -1;

        // D-pad or left stick, whichever moved. The stick is edge-detected the
        // same way the pad is: crossing the deadzone counts as a press.
        int dir = -1;
        if (in.buttons[input::InputState::DPadUp]    || in.axes[input::InputState::LY] >  kStickDeadzone) dir = NavUp;
        else if (in.buttons[input::InputState::DPadDown]  || in.axes[input::InputState::LY] < -kStickDeadzone) dir = NavDown;
        else if (in.buttons[input::InputState::DPadLeft]  || in.axes[input::InputState::LX] < -kStickDeadzone) dir = NavLeft;
        else if (in.buttons[input::InputState::DPadRight] || in.axes[input::InputState::LX] >  kStickDeadzone) dir = NavRight;

        if (dir < 0)
        {
            m_navHeld = -1;
            m_navTimer = 0.0f;
        }
        else if (dir != m_navHeld)
        {
            m_navHeld = dir;
            m_navTimer = kNavInitialDelay;
            MoveFocus(dir);
        }
        else
        {
            m_navTimer -= dt;
            if (m_navTimer <= 0.0f)
            {
                m_navTimer = kNavRepeatDelay;
                MoveFocus(dir);
            }
        }

        // Confirm / cancel fire on the press edge only. Cancel is raised even
        // with nothing focused — "back out of this menu" is a screen-level
        // action, so it goes to the root and a script can hang on_cancel there.
        const bool confirm = in.buttons[input::InputState::A];
        const bool cancel  = in.buttons[input::InputState::B];
        if (confirm && !m_prevConfirm && m_focus >= 0 && Navigable(m_focus))
            RaiseEvent(EvConfirm, m_focus);
        if (cancel && !m_prevCancel)
            RaiseEvent(EvCancel, m_focus >= 0 ? m_focus : kRootIndex);
        m_prevConfirm = confirm;
        m_prevCancel  = cancel;
    }

    const DrawList& Context::Emit()
    {
        m_draw.Clear();
        if (!m_assets)
            return m_draw;
        Solve();
        EmitWidget(kRootIndex);
        BuildBatches();
        // Time is spent, not just read: a second Emit in the same frame (a host
        // replaying the pass per tile band, say) must redraw the same frame
        // rather than step the animation again.
        m_dt = 0.0f;
        return m_draw;
    }

    void Context::EmitWidget(int index)
    {
        Widget& w = m_widgets[index];
        if (!w.alive || !w.visible)
            return;   // hiding a container hides its whole subtree

        if (index != kRootIndex)
        {
            // Focused widgets swap to their highlight colours. Doing it here
            // (one place, at emit) keeps every widget kind highlighting
            // identically. A Label has no background, so its glyphs use `color`
            // directly; anything with a background draws glyphs in textColor so
            // a focused button's caption stays readable over its highlight.
            const bool focused = w.focusable && m_focus == index;
            const float* color = focused ? w.focusColor : w.color;
            if (w.kind != KindLabel)
                EmitBackground(w, color);
            if (w.kind != KindImage && !w.text.empty() && !w.fontPath.empty())
            {
                const float* glyphColor = w.kind == KindLabel
                    ? color : (focused ? w.focusTextColor : w.textColor);
                EmitText(w, glyphColor);
            }
        }

        const std::vector<int> children = w.children;
        for (size_t i = 0; i < children.size(); ++i)
            EmitWidget(children[i]);
    }

    // Takes a mutable widget because an animated GIF's playback clock lives on
    // the widget and is stepped right here, where the host is reachable.
    void Context::EmitBackground(Widget& w, const float* color)
    {
        if (w.rect.Width() <= 0.0f || w.rect.Height() <= 0.0f)
            return;

        // An animated GIF steps its own clock and then draws as one plain
        // textured quad. Which resource that frame lives in is the host's
        // business — see HostAssets::AcquireGifFrame. 9-slice is skipped: a
        // sliced animation would need nine sub-rects per frame for no real gain.
        const bool isGif = !w.texturePath.empty() && gifanim::IsGifPath(w.texturePath);
        if (isGif)
        {
            int   gifTextureId = -1;
            float gifSlice     = -1.0f;
            // Advance needs the frame count, which only the host knows, so step
            // with the info returned for the CURRENT frame and use the result
            // next call. One frame of latency on a menu animation is invisible,
            // and it keeps the core free of any decode.
            const gifanim::Info* info = m_assets->AcquireGifFrame(
                w.texturePath.c_str(), w.gifPlay.frame, gifTextureId, gifSlice);
            if (info && gifTextureId >= 0)
            {
                gifanim::Advance(w.gifPlay, *info, w.playMode, m_dt);
                Quad q;
                q.x0 = w.rect.x0; q.y0 = w.rect.y0;
                q.x1 = w.rect.x1; q.y1 = w.rect.y1;
                q.kind = QuadTexture;
                q.textureId = gifTextureId;
                q.gifSlice = gifSlice;
                CopyColor(q.rgba, color);
                m_draw.quads.push_back(q);
                return;
            }
            // Not resident yet — the console streams. Fall through to the flat
            // colour below, but NEVER to AcquireTexture: asking the host to
            // treat a GIF path as a plain texture registers it in the wrong
            // cache and permanently breaks that GIF for the whole title.
        }

        int textureId = -1;
        if (!isGif && !w.texturePath.empty())
            textureId = m_assets->AcquireTexture(w.texturePath.c_str());

        if (textureId >= 0)
        {
            int texW = 0, texH = 0;
            if (w.slice > 0.0f && m_assets->TextureSize(textureId, texW, texH) &&
                texW > 0 && texH > 0)
            {
                EmitNineSlice(w, color, textureId, texW, texH);
                return;
            }
            Quad q;
            q.x0 = w.rect.x0; q.y0 = w.rect.y0;
            q.x1 = w.rect.x1; q.y1 = w.rect.y1;
            q.kind = QuadTexture;
            q.textureId = textureId;
            CopyColor(q.rgba, color);
            m_draw.quads.push_back(q);
            return;
        }

        // No texture (or not resident yet). An Image is nothing but its
        // texture, so it draws nothing; a Panel/Button falls back to its flat
        // color, which is also what keeps a menu usable while the atlas
        // streams in on the console.
        if (w.kind == KindImage || color[3] <= 0.0f)
            return;
        Quad q;
        q.x0 = w.rect.x0; q.y0 = w.rect.y0;
        q.x1 = w.rect.x1; q.y1 = w.rect.y1;
        q.kind = QuadSolid;
        q.textureId = -1;
        CopyColor(q.rgba, color);
        m_draw.quads.push_back(q);
    }

    // Nine-slice: the border keeps its authored pixel size while the middle
    // stretches, so one 64x64 source works at any panel size. Borders are
    // clamped to half the destination extent, so a small panel degrades to a
    // squashed frame rather than overlapping cells.
    void Context::EmitNineSlice(const Widget& w, const float* color,
                                int textureId, int texW, int texH)
    {
        const float destW = w.rect.Width();
        const float destH = w.rect.Height();

        float sx = w.slice;
        float sy = w.slice;
        if (sx * 2.0f > destW) sx = destW * 0.5f;
        if (sy * 2.0f > destH) sy = destH * 0.5f;

        float su = w.slice;
        float sv = w.slice;
        if (su * 2.0f > (float)texW) su = (float)texW * 0.5f;
        if (sv * 2.0f > (float)texH) sv = (float)texH * 0.5f;

        const float xs[4] = { w.rect.x0, w.rect.x0 + sx, w.rect.x1 - sx, w.rect.x1 };
        const float ys[4] = { w.rect.y0, w.rect.y0 + sy, w.rect.y1 - sy, w.rect.y1 };
        const float us[4] = { 0.0f, su / (float)texW, 1.0f - su / (float)texW, 1.0f };
        const float vs[4] = { 0.0f, sv / (float)texH, 1.0f - sv / (float)texH, 1.0f };

        for (int row = 0; row < 3; ++row)
        {
            for (int col = 0; col < 3; ++col)
            {
                if (xs[col + 1] <= xs[col] || ys[row + 1] <= ys[row])
                    continue;   // degenerate cell (border ate the middle)
                Quad q;
                q.x0 = xs[col];     q.y0 = ys[row];
                q.x1 = xs[col + 1]; q.y1 = ys[row + 1];
                q.u0 = us[col];     q.v0 = vs[row];
                q.u1 = us[col + 1]; q.v1 = vs[row + 1];
                q.kind = QuadTexture;
                q.textureId = textureId;
                CopyColor(q.rgba, color);
                m_draw.quads.push_back(q);
            }
        }
    }

    void Context::EmitText(const Widget& w, const float* color)
    {
        const text::FontMetrics* font = m_assets->AcquireFont(w.fontPath.c_str());
        const int atlas = m_assets->AcquireFontAtlas(w.fontPath.c_str());
        if (!font || atlas < 0 || font->sourcePixelSize <= 0.0f)
            return;

        text::Layout layout;
        const float wrapWidth = w.wrap ? w.rect.Width() : 0.0f;
        if (!text::BuildLayout(*font, w.text, w.fontSize, wrapWidth, layout) ||
            layout.glyphs.empty())
            return;

        // TextLayout is always left-aligned and top-anchored — deliberately, so
        // the shipped Text attribute keeps its exact behaviour. Alignment for
        // the GUI is done here, by offsetting the whole block against the
        // widget rect using the measured extents.
        float ox = w.rect.x0;
        if (w.alignH == AlignCenterH)   ox = w.rect.CenterX() - layout.width * 0.5f;
        else if (w.alignH == AlignRight) ox = w.rect.x1 - layout.width;

        float oy = w.rect.y0;
        if (w.alignV == AlignMiddle)     oy = w.rect.CenterY() - layout.height * 0.5f;
        else if (w.alignV == AlignBottom) oy = w.rect.y1 - layout.height;

        // Positions come out of BuildLayout already at the requested pixel
        // size; the glyph's own extents are in source-atlas units and need the
        // size ratio. Same split as the Text attribute overlay.
        const float glyphScale = w.fontSize / font->sourcePixelSize;
        const size_t glyphCount = font->glyphs.size();

        for (size_t i = 0; i < layout.glyphs.size(); ++i)
        {
            const text::PositionedGlyph& positioned = layout.glyphs[i];
            if (positioned.glyphIndex >= glyphCount)
                continue;
            const text::Glyph& glyph = font->glyphs[positioned.glyphIndex];

            Quad q;
            q.x0 = ox + positioned.x;
            q.y0 = oy + positioned.y;
            q.x1 = q.x0 + glyph.width * glyphScale;
            q.y1 = q.y0 + glyph.height * glyphScale;
            q.u0 = glyph.u0; q.v0 = glyph.v0;
            q.u1 = glyph.u1; q.v1 = glyph.v1;
            q.kind = QuadGlyph;
            q.textureId = atlas;
            CopyColor(q.rgba, color);
            m_draw.quads.push_back(q);
        }
    }

    // Merge consecutive quads that share kind, texture and color into one draw
    // call each. Order is never changed — merging only ever collapses adjacent
    // runs, so painter's order survives. A label becomes exactly one batch.
    void Context::BuildBatches()
    {
        for (size_t i = 0; i < m_draw.quads.size(); ++i)
        {
            const Quad& q = m_draw.quads[i];
            if (!m_draw.batches.empty())
            {
                Batch& last = m_draw.batches.back();
                // gifSlice is part of the identity: two widgets on the same GIF
                // share a texture id on the console but sit on different
                // frames, and merging them would draw both at one frame.
                if (last.kind == q.kind && last.textureId == q.textureId &&
                    last.gifSlice == q.gifSlice && SameColor(last.rgba, q.rgba))
                {
                    ++last.count;
                    continue;
                }
            }
            Batch batch;
            batch.first = (int)i;
            batch.count = 1;
            batch.kind = q.kind;
            batch.textureId = q.textureId;
            batch.gifSlice = q.gifSlice;
            CopyColor(batch.rgba, q.rgba);
            m_draw.batches.push_back(batch);
        }
    }
}
