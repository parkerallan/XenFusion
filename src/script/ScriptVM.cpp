#include "script/ScriptVM.h"
#include "script/ScriptTypes.h"
#include "physics/PhysicsWorld.h"
#include "gui/GuiContext.h"

extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}

#include <string.h>

#include <string>
#include <vector>

// Shared Lua 5.4 wrapper. Strict C++03 (compiled by the XDK too). Only this file
// touches Lua. Engine pointers live in the Lua registry so the C API functions
// can reach the PhysicsWorld / ScriptHost without a global.

namespace script
{
    struct ScriptVM::Impl
    {
        lua_State*          L;
        phys::PhysicsWorld* phys;
        ScriptHost*         host;
        gui::Context*       gui;
        std::vector<int>    scripted; // objectIndices with a loaded script

        Impl() : L(0), phys(0), host(0), gui(0) {}
    };
}

namespace
{
    phys::PhysicsWorld* getPhys(lua_State* L)
    {
        lua_getfield(L, LUA_REGISTRYINDEX, "xf_phys");
        phys::PhysicsWorld* p = (phys::PhysicsWorld*)lua_touserdata(L, -1);
        lua_pop(L, 1);
        return p;
    }
    script::ScriptHost* getHost(lua_State* L)
    {
        lua_getfield(L, LUA_REGISTRYINDEX, "xf_host");
        script::ScriptHost* h = (script::ScriptHost*)lua_touserdata(L, -1);
        lua_pop(L, 1);
        return h;
    }
    int getSelfId(lua_State* L, int idx)
    {
        // Stored under "__id" (not "id") so it doesn't shadow the id() method:
        // Lua checks table fields before the metatable's __index.
        lua_getfield(L, idx, "__id");
        int id = (int)lua_tointeger(L, -1);
        lua_pop(L, 1);
        return id;
    }

    // Push a fresh object handle { __id = objectIndex } with the shared method
    // metatable, so scripts can call self:apply_impulse(...), self:id(), etc.
    void pushObject(lua_State* L, int objectIndex)
    {
        lua_createtable(L, 0, 1);
        lua_pushinteger(L, objectIndex);
        lua_setfield(L, -2, "__id");
        luaL_getmetatable(L, "xf_object");
        lua_setmetatable(L, -2);
    }

    // --- object methods (self is arg 1) ---
    int obj_apply_impulse(lua_State* L)
    {
        phys::PhysicsWorld* p = getPhys(L);
        if (p) p->ApplyImpulse(getSelfId(L, 1),
                               (float)luaL_optnumber(L, 2, 0),
                               (float)luaL_optnumber(L, 3, 0),
                               (float)luaL_optnumber(L, 4, 0));
        return 0;
    }
    int obj_set_velocity(lua_State* L)
    {
        phys::PhysicsWorld* p = getPhys(L);
        if (p) p->SetLinearVelocity(getSelfId(L, 1),
                                    (float)luaL_optnumber(L, 2, 0),
                                    (float)luaL_optnumber(L, 3, 0),
                                    (float)luaL_optnumber(L, 4, 0));
        return 0;
    }
    int obj_set_transform(lua_State* L)
    {
        const int id = getSelfId(L, 1);
        float pos[3] = { (float)luaL_optnumber(L, 2, 0), (float)luaL_optnumber(L, 3, 0), (float)luaL_optnumber(L, 4, 0) };
        float rot[3] = { (float)luaL_optnumber(L, 5, 0), (float)luaL_optnumber(L, 6, 0), (float)luaL_optnumber(L, 7, 0) };
        phys::PhysicsWorld* p = getPhys(L);
        if (p) p->SetTransform(id, pos, rot);
        return 0;
    }
    int obj_position(lua_State* L)
    {
        phys::PhysicsWorld* p = getPhys(L);
        float o[3] = { 0, 0, 0 };
        if (p) p->GetPosition(getSelfId(L, 1), o);
        lua_pushnumber(L, o[0]); lua_pushnumber(L, o[1]); lua_pushnumber(L, o[2]);
        return 3;
    }
    int obj_velocity(lua_State* L)
    {
        phys::PhysicsWorld* p = getPhys(L);
        float v[3] = { 0, 0, 0 };
        if (p) p->GetLinearVelocity(getSelfId(L, 1), v);
        lua_pushnumber(L, v[0]); lua_pushnumber(L, v[1]); lua_pushnumber(L, v[2]);
        return 3;
    }
    int obj_id(lua_State* L)
    {
        lua_pushinteger(L, getSelfId(L, 1));
        return 1;
    }
    int obj_name(lua_State* L)
    {
        script::ScriptHost* h = getHost(L);
        const char* n = h ? h->ObjectName(getSelfId(L, 1)) : "";
        lua_pushstring(L, n ? n : "");
        return 1;
    }

    // --- globals ---
    int l_input_button(lua_State* L)
    {
        script::ScriptHost* h = getHost(L);
        lua_pushboolean(L, h ? h->InputButton(luaL_checkstring(L, 1)) : 0);
        return 1;
    }
    int l_input_axis(lua_State* L)
    {
        script::ScriptHost* h = getHost(L);
        lua_pushnumber(L, h ? h->InputAxis(luaL_checkstring(L, 1)) : 0.0);
        return 1;
    }
    int l_log(lua_State* L)
    {
        script::ScriptHost* h = getHost(L);
        const char* s = luaL_tolstring(L, 1, NULL); // coerce any value to a string
        if (h) h->Log(s);
        lua_pop(L, 1);
        return 0;
    }
    int l_find(lua_State* L)
    {
        script::ScriptHost* h = getHost(L);
        const int id = h ? h->FindObject(luaL_checkstring(L, 1)) : -1;
        if (id < 0) lua_pushnil(L);
        else        pushObject(L, id);
        return 1;
    }

    int l_text_set(lua_State* L)
    {
        script::ScriptHost* h = getHost(L);
        if (h) h->TextSetValue(h->FindObject(luaL_checkstring(L, 1)),
                               luaL_checkstring(L, 2));
        return 0;
    }

    // --- video table: control an object's Video attribute by object name ---
    int l_video_play(lua_State* L) // play(name [, loop]) — once unless loop is true
    {
        script::ScriptHost* h = getHost(L);
        if (h) h->VideoSetPlaying(h->FindObject(luaL_checkstring(L, 1)), true,
                                  lua_toboolean(L, 2) != 0);
        return 0;
    }
    int l_video_stop(lua_State* L)
    {
        script::ScriptHost* h = getHost(L);
        if (h) h->VideoSetPlaying(h->FindObject(luaL_checkstring(L, 1)), false, false);
        return 0;
    }
    int l_video_is_playing(lua_State* L)
    {
        script::ScriptHost* h = getHost(L);
        lua_pushboolean(L, h ? (h->VideoIsPlaying(h->FindObject(luaL_checkstring(L, 1))) ? 1 : 0) : 0);
        return 1;
    }

    // --- audio table: control an object's Audio attribute by object name ---
    int l_audio_play(lua_State* L)
    {
        script::ScriptHost* h = getHost(L);
        if (h) h->AudioSetPlaying(h->FindObject(luaL_checkstring(L, 1)), true);
        return 0;
    }
    int l_audio_stop(lua_State* L)
    {
        script::ScriptHost* h = getHost(L);
        if (h) h->AudioSetPlaying(h->FindObject(luaL_checkstring(L, 1)), false);
        return 0;
    }
    int l_audio_is_playing(lua_State* L)
    {
        script::ScriptHost* h = getHost(L);
        lua_pushboolean(L, h ? (h->AudioIsPlaying(h->FindObject(luaL_checkstring(L, 1))) ? 1 : 0) : 0);
        return 1;
    }
    int l_audio_set_volume(lua_State* L)
    {
        script::ScriptHost* h = getHost(L);
        if (h) h->AudioSetVolume(h->FindObject(luaL_checkstring(L, 1)),
                                 (float)luaL_checknumber(L, 2));
        return 0;
    }
    int l_audio_set_pitch(lua_State* L)
    {
        script::ScriptHost* h = getHost(L);
        if (h) h->AudioSetPitch(h->FindObject(luaL_checkstring(L, 1)),
                                (float)luaL_checknumber(L, 2));
        return 0;
    }
    int l_audio_set_loop(lua_State* L)
    {
        script::ScriptHost* h = getHost(L);
        if (h) h->AudioSetLoop(h->FindObject(luaL_checkstring(L, 1)),
                               lua_toboolean(L, 2) != 0);
        return 0;
    }

    int l_animator_set_float(lua_State* L)
    {
        script::ScriptHost* h = getHost(L);
        if (h) h->AnimatorSetFloat(h->FindObject(luaL_checkstring(L, 1)),
                                   luaL_checkstring(L, 2), (float)luaL_checknumber(L, 3));
        return 0;
    }
    int l_animator_set_bool(lua_State* L)
    {
        script::ScriptHost* h = getHost(L);
        if (h) h->AnimatorSetBool(h->FindObject(luaL_checkstring(L, 1)),
                                  luaL_checkstring(L, 2), lua_toboolean(L, 3) != 0);
        return 0;
    }
    int l_animator_set_trigger(lua_State* L)
    {
        script::ScriptHost* h = getHost(L);
        if (h) h->AnimatorSetTrigger(h->FindObject(luaL_checkstring(L, 1)), luaL_checkstring(L, 2));
        return 0;
    }
    int l_animator_set_state(lua_State* L)
    {
        script::ScriptHost* h = getHost(L);
        if (h) h->AnimatorSetState(h->FindObject(luaL_checkstring(L, 1)), luaL_checkstring(L, 2));
        return 0;
    }

    // ---------------------------------------------------------------------
    // gui table + xf_widget handles
    //
    // Unlike video/audio, the GUI needs no ScriptHost virtuals: gui::Context is
    // shared code that both targets compile, so these bind straight to it. A
    // widget handle is { __wi = index, __wg = generation }; the generation is
    // what makes a handle to a destroyed widget fail safely instead of aliasing
    // whatever later reused the slot.
    // ---------------------------------------------------------------------

    gui::Context* getGui(lua_State* L)
    {
        lua_getfield(L, LUA_REGISTRYINDEX, "xf_gui");
        gui::Context* g = (gui::Context*)lua_touserdata(L, -1);
        lua_pop(L, 1);
        return g;
    }

    void pushWidget(lua_State* L, gui::Handle h)
    {
        if (!h.Valid()) { lua_pushnil(L); return; }
        lua_createtable(L, 0, 2);
        lua_pushinteger(L, h.index);
        lua_setfield(L, -2, "__wi");
        lua_pushinteger(L, (lua_Integer)h.generation);
        lua_setfield(L, -2, "__wg");
        luaL_getmetatable(L, "xf_widget");
        lua_setmetatable(L, -2);
    }

    gui::Handle toWidget(lua_State* L, int idx)
    {
        gui::Handle h;
        if (!lua_istable(L, idx))
            return h;
        lua_getfield(L, idx, "__wi");
        if (!lua_isnumber(L, -1)) { lua_pop(L, 1); return h; }
        const int index = (int)lua_tointeger(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, idx, "__wg");
        const unsigned int gen = (unsigned int)lua_tointeger(L, -1);
        lua_pop(L, 1);
        return gui::Handle(index, gen);
    }

    // Named constants keep menu scripts readable; an unknown name falls back to
    // the default rather than erroring, so a typo degrades to top-left.
    int anchorFromName(const char* name, int fallback)
    {
        if (!name) return fallback;
        if (!strcmp(name, "topleft"))     return gui::AnchorTopLeft;
        if (!strcmp(name, "top"))         return gui::AnchorTop;
        if (!strcmp(name, "topright"))    return gui::AnchorTopRight;
        if (!strcmp(name, "left"))        return gui::AnchorLeft;
        if (!strcmp(name, "center"))      return gui::AnchorCenter;
        if (!strcmp(name, "right"))       return gui::AnchorRight;
        if (!strcmp(name, "bottomleft"))  return gui::AnchorBottomLeft;
        if (!strcmp(name, "bottom"))      return gui::AnchorBottom;
        if (!strcmp(name, "bottomright")) return gui::AnchorBottomRight;
        return fallback;
    }
    int alignHFromName(const char* name, int fallback)
    {
        if (!name) return fallback;
        if (!strcmp(name, "left"))   return gui::AlignLeft;
        if (!strcmp(name, "center")) return gui::AlignCenterH;
        if (!strcmp(name, "right"))  return gui::AlignRight;
        return fallback;
    }
    int alignVFromName(const char* name, int fallback)
    {
        if (!name) return fallback;
        if (!strcmp(name, "top"))    return gui::AlignTop;
        if (!strcmp(name, "middle")) return gui::AlignMiddle;
        if (!strcmp(name, "bottom")) return gui::AlignBottom;
        return fallback;
    }

    // Read opts.<key> = {r,g,b[,a]} (alpha defaults to 1). Returns false when
    // the key is absent, so the caller can leave the widget default alone.
    bool readColor(lua_State* L, int optsIdx, const char* key, float* out)
    {
        lua_getfield(L, optsIdx, key);
        if (!lua_istable(L, -1)) { lua_pop(L, 1); return false; }
        for (int i = 0; i < 4; ++i)
        {
            lua_geti(L, -1, i + 1);
            out[i] = lua_isnumber(L, -1) ? (float)lua_tonumber(L, -1) : 1.0f;
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
        return true;
    }

    // xf_gui_cb[widgetIndex] = { on_confirm = fn, ... }. Keyed by index alone:
    // Destroy clears the entry, so a recycled slot can never inherit a stale
    // callback.
    void guiCallbackTable(lua_State* L, int widgetIndex, bool create)
    {
        lua_getfield(L, LUA_REGISTRYINDEX, "xf_gui_cb");   // [cbs]
        lua_geti(L, -1, widgetIndex);                       // [cbs, entry]
        if (!lua_istable(L, -1) && create)
        {
            lua_pop(L, 1);
            lua_newtable(L);                                // [cbs, entry]
            lua_pushvalue(L, -1);                           // [cbs, entry, entry]
            lua_seti(L, -3, widgetIndex);                   // [cbs, entry]
        }
    }

    void storeCallback(lua_State* L, int optsIdx, int widgetIndex, const char* key)
    {
        lua_getfield(L, optsIdx, key);
        if (!lua_isfunction(L, -1)) { lua_pop(L, 1); return; }
        guiCallbackTable(L, widgetIndex, true);  // [fn, cbs, entry]
        lua_pushvalue(L, -3);                    // [fn, cbs, entry, fn]
        lua_setfield(L, -2, key);                // [fn, cbs, entry]
        lua_pop(L, 3);
    }

    void clearCallbacks(lua_State* L, int widgetIndex)
    {
        lua_getfield(L, LUA_REGISTRYINDEX, "xf_gui_cb");
        lua_pushnil(L);
        lua_seti(L, -2, widgetIndex);
        lua_pop(L, 1);
    }

    // Call widget[handler](widget) if the widget still has one. Errors are
    // reported and swallowed — one bad menu callback must not kill the VM.
    void fireCallback(lua_State* L, gui::Handle h, const char* handler)
    {
        guiCallbackTable(L, h.index, false);   // [cbs, entry]
        if (!lua_istable(L, -1)) { lua_pop(L, 2); return; }
        lua_getfield(L, -1, handler);          // [cbs, entry, fn]
        if (!lua_isfunction(L, -1)) { lua_pop(L, 3); return; }
        pushWidget(L, h);                      // [cbs, entry, fn, widget]
        if (lua_pcall(L, 1, 0, 0) != LUA_OK)
        {
            script::ScriptHost* host = getHost(L);
            if (host)
                host->LogError((std::string("gui ") + handler + ": " +
                                lua_tostring(L, -1)).c_str());
            lua_pop(L, 1);
        }
        lua_pop(L, 2);                         // entry + cbs
    }

    // Apply every recognised key of an options table to a freshly created
    // widget. Absent keys leave the widget's default in place.
    void applyOptions(lua_State* L, int optsIdx, gui::Context* g, gui::Handle h)
    {
        if (!lua_istable(L, optsIdx))
            return;

        lua_getfield(L, optsIdx, "anchor");
        if (lua_isstring(L, -1))
            g->SetAnchor(h, anchorFromName(lua_tostring(L, -1), gui::AnchorTopLeft));
        lua_pop(L, 1);

        lua_getfield(L, optsIdx, "x");
        lua_getfield(L, optsIdx, "y");
        if (lua_isnumber(L, -2) || lua_isnumber(L, -1))
            g->SetPos(h, (float)lua_tonumber(L, -2), (float)lua_tonumber(L, -1));
        lua_pop(L, 2);

        lua_getfield(L, optsIdx, "w");
        lua_getfield(L, optsIdx, "h");
        if (lua_isnumber(L, -2) && lua_isnumber(L, -1))
            g->SetSize(h, (float)lua_tonumber(L, -2), (float)lua_tonumber(L, -1));
        lua_pop(L, 2);

        float color[4];
        if (readColor(L, optsIdx, "color", color))
            g->SetColor(h, color[0], color[1], color[2], color[3]);
        if (readColor(L, optsIdx, "focus_color", color))
            g->SetFocusColor(h, color[0], color[1], color[2], color[3]);
        if (readColor(L, optsIdx, "text_color", color))
            g->SetTextColor(h, color[0], color[1], color[2], color[3]);
        if (readColor(L, optsIdx, "focus_text_color", color))
            g->SetFocusTextColor(h, color[0], color[1], color[2], color[3]);

        lua_getfield(L, optsIdx, "texture");
        if (lua_isstring(L, -1)) g->SetTexture(h, lua_tostring(L, -1));
        lua_pop(L, 1);

        lua_getfield(L, optsIdx, "slice");
        if (lua_isnumber(L, -1)) g->SetSlice(h, (float)lua_tonumber(L, -1));
        lua_pop(L, 1);

        lua_getfield(L, optsIdx, "text");
        if (lua_isstring(L, -1)) g->SetText(h, lua_tostring(L, -1));
        lua_pop(L, 1);

        lua_getfield(L, optsIdx, "font");
        if (lua_isstring(L, -1)) g->SetFont(h, lua_tostring(L, -1));
        lua_pop(L, 1);

        lua_getfield(L, optsIdx, "size");
        if (lua_isnumber(L, -1)) g->SetFontSize(h, (float)lua_tonumber(L, -1));
        lua_pop(L, 1);

        lua_getfield(L, optsIdx, "align");
        lua_getfield(L, optsIdx, "valign");
        if (lua_isstring(L, -2) || lua_isstring(L, -1))
        {
            const int ah = alignHFromName(lua_tostring(L, -2), gui::AlignLeft);
            const int av = alignVFromName(lua_tostring(L, -1), gui::AlignTop);
            g->SetAlign(h, ah, av);
        }
        lua_pop(L, 2);

        lua_getfield(L, optsIdx, "wrap");
        if (!lua_isnil(L, -1)) g->SetWrap(h, lua_toboolean(L, -1) != 0);
        lua_pop(L, 1);

        lua_getfield(L, optsIdx, "visible");
        if (!lua_isnil(L, -1)) g->SetVisible(h, lua_toboolean(L, -1) != 0);
        lua_pop(L, 1);

        lua_getfield(L, optsIdx, "enabled");
        if (!lua_isnil(L, -1)) g->SetEnabled(h, lua_toboolean(L, -1) != 0);
        lua_pop(L, 1);

        lua_getfield(L, optsIdx, "focusable");
        if (!lua_isnil(L, -1)) g->SetFocusable(h, lua_toboolean(L, -1) != 0);
        lua_pop(L, 1);

        storeCallback(L, optsIdx, h.index, "on_confirm");
        storeCallback(L, optsIdx, h.index, "on_cancel");
        storeCallback(L, optsIdx, h.index, "on_focus");
    }

    int guiCreate(lua_State* L, int kind)
    {
        gui::Context* g = getGui(L);
        if (!g) { lua_pushnil(L); return 1; }

        gui::Handle parent;
        if (lua_istable(L, 1))
        {
            lua_getfield(L, 1, "parent");
            parent = toWidget(L, lua_gettop(L));
            lua_pop(L, 1);
        }
        const gui::Handle h = g->Create(kind, parent);
        if (!h.Valid())
            return luaL_error(L, "gui: widget limit reached");
        // A slot can be recycled from a destroyed widget (including one
        // destroyed as part of a subtree), so wipe any callbacks left on this
        // index before applying this widget's own. Clearing here rather than at
        // destroy time is what makes recycling correct in every order.
        clearCallbacks(L, h.index);
        applyOptions(L, 1, g, h);
        pushWidget(L, h);
        return 1;
    }

    int l_gui_panel(lua_State* L)  { return guiCreate(L, gui::KindPanel); }
    int l_gui_label(lua_State* L)  { return guiCreate(L, gui::KindLabel); }
    int l_gui_image(lua_State* L)  { return guiCreate(L, gui::KindImage); }
    int l_gui_button(lua_State* L) { return guiCreate(L, gui::KindButton); }

    int l_gui_set_focus(lua_State* L)
    {
        gui::Context* g = getGui(L);
        if (g) g->SetFocus(toWidget(L, 1));
        return 0;
    }
    int l_gui_focus(lua_State* L)
    {
        gui::Context* g = getGui(L);
        if (!g) { lua_pushnil(L); return 1; }
        pushWidget(L, g->Focus());
        return 1;
    }
    int l_gui_set_paused(lua_State* L)
    {
        gui::Context* g = getGui(L);
        if (g) g->SetPaused(lua_toboolean(L, 1) != 0);
        return 0;
    }
    int l_gui_is_paused(lua_State* L)
    {
        gui::Context* g = getGui(L);
        lua_pushboolean(L, g && g->Paused() ? 1 : 0);
        return 1;
    }
    int l_gui_clear(lua_State* L)
    {
        gui::Context* g = getGui(L);
        if (g) g->Clear();
        lua_newtable(L);
        lua_setfield(L, LUA_REGISTRYINDEX, "xf_gui_cb");
        return 0;
    }
    int l_gui_root(lua_State* L)
    {
        gui::Context* g = getGui(L);
        if (!g) { lua_pushnil(L); return 1; }
        pushWidget(L, g->Root());
        return 1;
    }

    // --- widget methods (self is arg 1) ---
    int w_set_visible(lua_State* L)
    {
        gui::Context* g = getGui(L);
        if (g) g->SetVisible(toWidget(L, 1), lua_toboolean(L, 2) != 0);
        return 0;
    }
    int w_set_enabled(lua_State* L)
    {
        gui::Context* g = getGui(L);
        if (g) g->SetEnabled(toWidget(L, 1), lua_toboolean(L, 2) != 0);
        return 0;
    }
    int w_set_pos(lua_State* L)
    {
        gui::Context* g = getGui(L);
        if (g) g->SetPos(toWidget(L, 1), (float)luaL_optnumber(L, 2, 0),
                         (float)luaL_optnumber(L, 3, 0));
        return 0;
    }
    int w_set_size(lua_State* L)
    {
        gui::Context* g = getGui(L);
        if (g) g->SetSize(toWidget(L, 1), (float)luaL_optnumber(L, 2, 0),
                          (float)luaL_optnumber(L, 3, 0));
        return 0;
    }
    int w_set_anchor(lua_State* L)
    {
        gui::Context* g = getGui(L);
        if (g) g->SetAnchor(toWidget(L, 1),
                            anchorFromName(luaL_checkstring(L, 2), gui::AnchorTopLeft));
        return 0;
    }
    int w_set_color(lua_State* L)
    {
        gui::Context* g = getGui(L);
        if (g) g->SetColor(toWidget(L, 1), (float)luaL_optnumber(L, 2, 1),
                           (float)luaL_optnumber(L, 3, 1),
                           (float)luaL_optnumber(L, 4, 1),
                           (float)luaL_optnumber(L, 5, 1));
        return 0;
    }
    int w_set_focus_color(lua_State* L)
    {
        gui::Context* g = getGui(L);
        if (g) g->SetFocusColor(toWidget(L, 1), (float)luaL_optnumber(L, 2, 1),
                                (float)luaL_optnumber(L, 3, 1),
                                (float)luaL_optnumber(L, 4, 1),
                                (float)luaL_optnumber(L, 5, 1));
        return 0;
    }
    int w_set_text_color(lua_State* L)
    {
        gui::Context* g = getGui(L);
        if (g) g->SetTextColor(toWidget(L, 1), (float)luaL_optnumber(L, 2, 1),
                               (float)luaL_optnumber(L, 3, 1),
                               (float)luaL_optnumber(L, 4, 1),
                               (float)luaL_optnumber(L, 5, 1));
        return 0;
    }
    int w_set_focus_text_color(lua_State* L)
    {
        gui::Context* g = getGui(L);
        if (g) g->SetFocusTextColor(toWidget(L, 1), (float)luaL_optnumber(L, 2, 1),
                                    (float)luaL_optnumber(L, 3, 1),
                                    (float)luaL_optnumber(L, 4, 1),
                                    (float)luaL_optnumber(L, 5, 1));
        return 0;
    }
    int w_set_text(lua_State* L)
    {
        gui::Context* g = getGui(L);
        if (g) g->SetText(toWidget(L, 1), luaL_optstring(L, 2, ""));
        return 0;
    }
    int w_set_texture(lua_State* L)
    {
        gui::Context* g = getGui(L);
        if (g) g->SetTexture(toWidget(L, 1), luaL_optstring(L, 2, ""));
        return 0;
    }
    int w_set_font(lua_State* L)
    {
        gui::Context* g = getGui(L);
        if (g) g->SetFont(toWidget(L, 1), luaL_optstring(L, 2, ""));
        return 0;
    }
    int w_set_font_size(lua_State* L)
    {
        gui::Context* g = getGui(L);
        if (g) g->SetFontSize(toWidget(L, 1), (float)luaL_checknumber(L, 2));
        return 0;
    }
    int w_set_align(lua_State* L)
    {
        gui::Context* g = getGui(L);
        if (g) g->SetAlign(toWidget(L, 1),
                           alignHFromName(luaL_optstring(L, 2, "left"), gui::AlignLeft),
                           alignVFromName(luaL_optstring(L, 3, "top"), gui::AlignTop));
        return 0;
    }
    int w_set_wrap(lua_State* L)
    {
        gui::Context* g = getGui(L);
        if (g) g->SetWrap(toWidget(L, 1), lua_toboolean(L, 2) != 0);
        return 0;
    }
    int w_set_focusable(lua_State* L)
    {
        gui::Context* g = getGui(L);
        if (g) g->SetFocusable(toWidget(L, 1), lua_toboolean(L, 2) != 0);
        return 0;
    }
    int w_text(lua_State* L)
    {
        gui::Context* g = getGui(L);
        lua_pushstring(L, g ? g->Text(toWidget(L, 1)) : "");
        return 1;
    }
    int w_visible(lua_State* L)
    {
        gui::Context* g = getGui(L);
        lua_pushboolean(L, g && g->Visible(toWidget(L, 1)) ? 1 : 0);
        return 1;
    }
    int w_alive(lua_State* L)
    {
        gui::Context* g = getGui(L);
        lua_pushboolean(L, g && g->Alive(toWidget(L, 1)) ? 1 : 0);
        return 1;
    }
    int w_rect(lua_State* L) // x, y, w, h in 1280x720 reference space
    {
        gui::Context* g = getGui(L);
        if (!g) return 0;
        const gui::Rect r = g->SolvedRect(toWidget(L, 1));
        lua_pushnumber(L, r.x0); lua_pushnumber(L, r.y0);
        lua_pushnumber(L, r.Width()); lua_pushnumber(L, r.Height());
        return 4;
    }
    int w_destroy(lua_State* L)
    {
        gui::Context* g = getGui(L);
        if (!g) return 0;
        const gui::Handle h = toWidget(L, 1);
        if (g->Alive(h))
            clearCallbacks(L, h.index);
        g->Destroy(h);
        return 0;
    }
}

namespace script
{
    ScriptVM::ScriptVM() : m_impl(new Impl()) {}
    ScriptVM::~ScriptVM() { Clear(); delete m_impl; }

    bool ScriptVM::Empty() const { return m_impl->scripted.empty(); }

    void ScriptVM::Clear()
    {
        if (m_impl->L) { lua_close(m_impl->L); m_impl->L = 0; }
        m_impl->phys = 0;
        m_impl->host = 0;
        m_impl->gui  = 0;
        m_impl->scripted.clear();
    }

    void ScriptVM::Begin(phys::PhysicsWorld* phys, ScriptHost* host,
                         gui::Context* guiContext)
    {
        Clear();
        m_impl->phys = phys;
        m_impl->host = host;
        m_impl->gui  = guiContext;
        lua_State* L = m_impl->L = luaL_newstate();
        if (!L) return;

        // Sandboxed standard libraries (no io/os/package).
        luaL_requiref(L, LUA_GNAME,       luaopen_base,      1); lua_pop(L, 1);
        luaL_requiref(L, LUA_TABLIBNAME,  luaopen_table,     1); lua_pop(L, 1);
        luaL_requiref(L, LUA_STRLIBNAME,  luaopen_string,    1); lua_pop(L, 1);
        luaL_requiref(L, LUA_MATHLIBNAME, luaopen_math,      1); lua_pop(L, 1);
        luaL_requiref(L, LUA_COLIBNAME,   luaopen_coroutine, 1); lua_pop(L, 1);
        luaL_requiref(L, LUA_UTF8LIBNAME, luaopen_utf8,      1); lua_pop(L, 1);

        // Engine pointers for the C API functions.
        lua_pushlightuserdata(L, (void*)phys); lua_setfield(L, LUA_REGISTRYINDEX, "xf_phys");
        lua_pushlightuserdata(L, (void*)host); lua_setfield(L, LUA_REGISTRYINDEX, "xf_host");
        lua_pushlightuserdata(L, (void*)guiContext); lua_setfield(L, LUA_REGISTRYINDEX, "xf_gui");

        // Object handle metatable: __index -> a table of the object methods.
        luaL_newmetatable(L, "xf_object");
        lua_newtable(L);
        static const luaL_Reg objMethods[] = {
            { "apply_impulse", obj_apply_impulse },
            { "set_velocity",  obj_set_velocity },
            { "set_transform", obj_set_transform },
            { "position",      obj_position },
            { "velocity",      obj_velocity },
            { "id",            obj_id },
            { "name",          obj_name },
            { NULL, NULL }
        };
        luaL_setfuncs(L, objMethods, 0);
        lua_setfield(L, -2, "__index");
        lua_pop(L, 1); // metatable

        // Global input table + log/find.
        lua_newtable(L);
        lua_pushcfunction(L, l_input_button); lua_setfield(L, -2, "button");
        lua_pushcfunction(L, l_input_axis);   lua_setfield(L, -2, "axis");
        lua_setglobal(L, "input");
        lua_pushcfunction(L, l_log);  lua_setglobal(L, "log");
        lua_pushcfunction(L, l_find); lua_setglobal(L, "find");

        lua_newtable(L);
        lua_pushcfunction(L, l_text_set); lua_setfield(L, -2, "set");
        lua_setglobal(L, "text");

        // Global video table (Video attribute play control by object name).
        lua_newtable(L);
        lua_pushcfunction(L, l_video_play);       lua_setfield(L, -2, "play");
        lua_pushcfunction(L, l_video_stop);       lua_setfield(L, -2, "stop");
        lua_pushcfunction(L, l_video_is_playing); lua_setfield(L, -2, "is_playing");
        lua_setglobal(L, "video");

        // Global audio table (Audio attribute control by object name).
        lua_newtable(L);
        lua_pushcfunction(L, l_audio_play);       lua_setfield(L, -2, "play");
        lua_pushcfunction(L, l_audio_stop);       lua_setfield(L, -2, "stop");
        lua_pushcfunction(L, l_audio_is_playing); lua_setfield(L, -2, "is_playing");
        lua_pushcfunction(L, l_audio_set_volume); lua_setfield(L, -2, "set_volume");
        lua_pushcfunction(L, l_audio_set_pitch);  lua_setfield(L, -2, "set_pitch");
        lua_pushcfunction(L, l_audio_set_loop);   lua_setfield(L, -2, "set_loop");
        lua_setglobal(L, "audio");

        lua_newtable(L);
        lua_pushcfunction(L, l_animator_set_float);   lua_setfield(L, -2, "SetFloat");
        lua_pushcfunction(L, l_animator_set_bool);    lua_setfield(L, -2, "SetBool");
        lua_pushcfunction(L, l_animator_set_trigger); lua_setfield(L, -2, "SetTrigger");
        lua_pushcfunction(L, l_animator_set_state);   lua_setfield(L, -2, "SetState");
        lua_setglobal(L, "Animator");

        // Global gui table + the widget handle metatable. Only registered when
        // the host actually has a GUI backend, so `gui` being nil is a clear
        // signal rather than a set of silent no-ops.
        if (guiContext)
        {
            luaL_newmetatable(L, "xf_widget");
            lua_newtable(L);
            static const luaL_Reg widgetMethods[] = {
                { "set_visible",     w_set_visible },
                { "set_enabled",     w_set_enabled },
                { "set_pos",         w_set_pos },
                { "set_size",        w_set_size },
                { "set_anchor",      w_set_anchor },
                { "set_color",       w_set_color },
                { "set_focus_color", w_set_focus_color },
                { "set_text_color",  w_set_text_color },
                { "set_focus_text_color", w_set_focus_text_color },
                { "set_text",        w_set_text },
                { "set_texture",     w_set_texture },
                { "set_font",        w_set_font },
                { "set_font_size",   w_set_font_size },
                { "set_align",       w_set_align },
                { "set_wrap",        w_set_wrap },
                { "set_focusable",   w_set_focusable },
                { "text",            w_text },
                { "visible",         w_visible },
                { "alive",           w_alive },
                { "rect",            w_rect },
                { "destroy",         w_destroy },
                { NULL, NULL }
            };
            luaL_setfuncs(L, widgetMethods, 0);
            lua_setfield(L, -2, "__index");
            lua_pop(L, 1); // metatable

            lua_newtable(L);
            lua_pushcfunction(L, l_gui_panel);      lua_setfield(L, -2, "panel");
            lua_pushcfunction(L, l_gui_label);      lua_setfield(L, -2, "label");
            lua_pushcfunction(L, l_gui_image);      lua_setfield(L, -2, "image");
            lua_pushcfunction(L, l_gui_button);     lua_setfield(L, -2, "button");
            lua_pushcfunction(L, l_gui_root);       lua_setfield(L, -2, "root");
            lua_pushcfunction(L, l_gui_set_focus);  lua_setfield(L, -2, "set_focus");
            lua_pushcfunction(L, l_gui_focus);      lua_setfield(L, -2, "focus");
            lua_pushcfunction(L, l_gui_set_paused); lua_setfield(L, -2, "set_paused");
            lua_pushcfunction(L, l_gui_is_paused);  lua_setfield(L, -2, "is_paused");
            lua_pushcfunction(L, l_gui_clear);      lua_setfield(L, -2, "clear");
            lua_setglobal(L, "gui");

            // Registry table: widgetIndex -> { on_confirm = fn, ... }
            lua_newtable(L);
            lua_setfield(L, LUA_REGISTRYINDEX, "xf_gui_cb");
        }

        // Registry table: objectIndex -> per-script environment.
        lua_newtable(L);
        lua_setfield(L, LUA_REGISTRYINDEX, "xf_scripts");
    }

    void ScriptVM::LoadScript(int objectIndex, const std::string& source, const std::string& name)
    {
        lua_State* L = m_impl->L;
        if (!L) return;

        const std::string chunkname = "@" + name;
        if (luaL_loadbuffer(L, source.data(), source.size(), chunkname.c_str()) != LUA_OK)
        {
            if (m_impl->host)
                m_impl->host->LogError((std::string("script error: ") + lua_tostring(L, -1)).c_str());
            lua_pop(L, 1);
            return;
        }
        // env with metatable {__index = _G}, plus self.
        lua_newtable(L);                       // [chunk, env]
        lua_newtable(L);                       // [chunk, env, mt]
        lua_pushglobaltable(L);                // [chunk, env, mt, _G]
        lua_setfield(L, -2, "__index");        // mt.__index = _G  -> [chunk, env, mt]
        lua_setmetatable(L, -2);               // setmetatable(env, mt) -> [chunk, env]
        pushObject(L, objectIndex);            // [chunk, env, self]
        lua_setfield(L, -2, "self");           // env.self = self -> [chunk, env]

        // chunk's _ENV upvalue (1) = env (keep a copy of env on the stack).
        lua_pushvalue(L, -1);                  // [chunk, env, env]
        lua_setupvalue(L, -3, 1);              // set chunk _ENV, pops -> [chunk, env]

        // Store env in xf_scripts[objectIndex].
        lua_getfield(L, LUA_REGISTRYINDEX, "xf_scripts"); // [chunk, env, scripts]
        lua_pushvalue(L, -2);                  // [chunk, env, scripts, env]
        lua_seti(L, -2, objectIndex);          // scripts[idx]=env -> [chunk, env, scripts]
        lua_pop(L, 2);                         // -> [chunk]

        // Run the top level (defines on_start/on_update/on_trigger into env).
        if (lua_pcall(L, 0, 0, 0) != LUA_OK)
        {
            if (m_impl->host)
                m_impl->host->LogError((std::string("script error: ") + lua_tostring(L, -1)).c_str());
            lua_pop(L, 1);
            return;
        }
        m_impl->scripted.push_back(objectIndex);
    }

    // Fetch xf_scripts[objIdx].<handler>; if it's a function leave it on top with
    // `scripts` and `env` below it and return true, else clean up and return false.
    // Caller then pushes args and pcalls, and finally pops env + scripts.
    static bool beginCall(lua_State* L, int objIdx, const char* handler)
    {
        lua_getfield(L, LUA_REGISTRYINDEX, "xf_scripts"); // [scripts]
        lua_geti(L, -1, objIdx);                           // [scripts, env]
        if (!lua_istable(L, -1)) { lua_pop(L, 2); return false; }
        lua_getfield(L, -1, handler);                      // [scripts, env, fn]
        if (!lua_isfunction(L, -1)) { lua_pop(L, 3); return false; }
        return true; // stack: [scripts, env, fn]
    }

    void ScriptVM::Start()
    {
        lua_State* L = m_impl->L;
        if (!L) return;
        for (size_t i = 0; i < m_impl->scripted.size(); ++i)
        {
            if (!beginCall(L, m_impl->scripted[i], "on_start")) continue;
            if (lua_pcall(L, 0, 0, 0) != LUA_OK)
            {
                if (m_impl->host) m_impl->host->LogError((std::string("on_start: ") + lua_tostring(L, -1)).c_str());
                lua_pop(L, 1);
            }
            lua_pop(L, 2); // env + scripts
        }
    }

    void ScriptVM::Update(float dt)
    {
        lua_State* L = m_impl->L;
        if (!L) return;

        // Drain GUI events first: the host ran gui::Context::Update just before
        // this, so a confirm the player pressed this frame reaches its callback
        // before any on_update reads the world it just changed.
        if (m_impl->gui)
        {
            gui::Context::Event ev;
            while (m_impl->gui->PopEvent(ev))
            {
                const char* handler = ev.type == gui::Context::EvConfirm ? "on_confirm"
                                    : ev.type == gui::Context::EvCancel  ? "on_cancel"
                                                                         : "on_focus";
                fireCallback(L, ev.widget, handler);
            }
        }

        for (size_t i = 0; i < m_impl->scripted.size(); ++i)
        {
            if (!beginCall(L, m_impl->scripted[i], "on_update")) continue;
            lua_pushnumber(L, dt);
            if (lua_pcall(L, 1, 0, 0) != LUA_OK)
            {
                if (m_impl->host) m_impl->host->LogError((std::string("on_update: ") + lua_tostring(L, -1)).c_str());
                lua_pop(L, 1);
            }
            lua_pop(L, 2); // env + scripts
        }
    }

    void ScriptVM::FireTrigger(int objectIndex, int otherObjectIndex, const char* boneName)
    {
        lua_State* L = m_impl->L;
        if (!L) return;
        if (!beginCall(L, objectIndex, "on_trigger")) return;
        pushObject(L, otherObjectIndex);
        if (boneName && boneName[0]) lua_pushstring(L, boneName);
        else lua_pushnil(L);
        if (lua_pcall(L, 2, 0, 0) != LUA_OK)
        {
            if (m_impl->host) m_impl->host->LogError((std::string("on_trigger: ") + lua_tostring(L, -1)).c_str());
            lua_pop(L, 1);
        }
        lua_pop(L, 2); // env + scripts
    }
}
