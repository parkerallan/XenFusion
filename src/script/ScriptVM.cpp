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
    // Scheduler state behind the `time` table. Callbacks are held by a registry
    // ref (luaL_ref) so the collector can't take them while they wait. Named at
    // namespace scope, not inside Impl, because the C API functions below live in
    // an anonymous namespace and can't reach ScriptVM's private nested type.
    struct VmState
    {
        struct Timer
        {
            int    id;
            double due;       // absolute `now` at which it fires
            double interval;  // seconds between repeats (0 = one-shot)
            int    ref;       // LUA_REGISTRYINDEX ref to the callback
            bool   repeating;
            bool   dead;      // cancelled, or one-shot already fired
            Timer() : id(0), due(0.0), interval(0.0), ref(LUA_NOREF),
                      repeating(false), dead(false) {}
        };

        std::vector<Timer> timers;
        int    nextTimerId;
        double now;           // seconds since Begin (time.total)

        VmState() : nextTimerId(1), now(0.0) {}
    };

    struct ScriptVM::Impl
    {
        lua_State*          L;
        phys::PhysicsWorld* phys;
        ScriptHost*         host;
        gui::Context*       gui;
        std::vector<int>    scripted; // objectIndices with a loaded script
        VmState             vm;

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
    script::VmState* getVm(lua_State* L)
    {
        lua_getfield(L, LUA_REGISTRYINDEX, "xf_vm");
        script::VmState* v = (script::VmState*)lua_touserdata(L, -1);
        lua_pop(L, 1);
        return v;
    }
    // The raw slot index in the handle, without checking whether it is still the
    // object the handle was made for. Only obj:id() and the liveness check want
    // this; everything else goes through getSelfId.
    int getRawId(lua_State* L, int idx)
    {
        // Stored under "__id" (not "id") so it doesn't shadow the id() method:
        // Lua checks table fields before the metatable's __index.
        lua_getfield(L, idx, "__id");
        const int id = (int)lua_tointeger(L, -1);
        lua_pop(L, 1);
        return id;
    }

    // Is this handle still pointing at the object it was made for? A destroyed
    // object's slot can be reused by a later spawn, so the generation stamped
    // into the handle is what distinguishes "my object" from "whatever took its
    // place". Same scheme as the GUI widget handles.
    bool handleAlive(lua_State* L, int idx, int id)
    {
        script::ScriptHost* h = getHost(L);
        if (!h || id < 0) return false;
        if (!h->ObjectAlive(id)) return false;
        lua_getfield(L, idx, "__gen");
        const unsigned gen = (unsigned)lua_tointeger(L, -1);
        lua_pop(L, 1);
        return gen == h->ObjectGeneration(id);
    }

    // The slot a handle refers to, or -1 once that object is gone. Returning -1
    // makes every accessor a safe no-op: the physics lookups miss, and the host
    // calls fail their bounds check.
    int getSelfId(lua_State* L, int idx)
    {
        const int id = getRawId(L, idx);
        return handleAlive(L, idx, id) ? id : -1;
    }

    // Push a fresh object handle { __id = slot, __gen = generation } with the
    // shared method metatable, so scripts can call self:apply_impulse(...) etc.
    void pushObject(lua_State* L, int objectIndex)
    {
        script::ScriptHost* h = getHost(L);
        lua_createtable(L, 0, 2);
        lua_pushinteger(L, objectIndex);
        lua_setfield(L, -2, "__id");
        lua_pushinteger(L, (lua_Integer)(h ? h->ObjectGeneration(objectIndex) : 0));
        lua_setfield(L, -2, "__gen");
        luaL_getmetatable(L, "xf_object");
        lua_setmetatable(L, -2);
    }

    // An object's transform lives in one of two places: the PhysicsWorld owns it
    // when the object has a Rigid Body (writing the scene there would just be
    // overwritten by the next pose read-back), and the host's live scene owns it
    // otherwise. Every transform accessor routes through these two.
    bool readTransform(lua_State* L, int id, float pos[3], float rot[3], float scale[3])
    {
        pos[0] = pos[1] = pos[2] = 0.0f;
        rot[0] = rot[1] = rot[2] = 0.0f;
        scale[0] = scale[1] = scale[2] = 1.0f;

        script::ScriptHost* h = getHost(L);
        const bool haveScene = h && h->GetObjectTransform(id, pos, rot, scale);

        phys::PhysicsWorld* p = getPhys(L);
        if (p && p->HasBody(id))
        {
            // The body is authoritative for position/rotation; scale is never
            // simulated, so it still comes from the scene.
            p->GetPosition(id, pos);
            p->GetRotation(id, rot);
            return true;
        }
        return haveScene;
    }
    void writeTransform(lua_State* L, int id, const float pos[3], const float rot[3],
                        const float scale[3], int mask)
    {
        phys::PhysicsWorld* p = getPhys(L);
        script::ScriptHost* h = getHost(L);
        const bool simulated = p && p->HasBody(id);

        if (simulated && (mask & (script::ScriptHost::XformPos |
                                  script::ScriptHost::XformRot)))
        {
            p->SetTransform(id, pos, rot);
            // The pose read-back overrides the draw transform anyway, so writing
            // position/rotation to the scene as well would be redundant — and
            // worse, it would mark the scene dirty every frame for a kinematic
            // body a script drives, forcing a full re-derive each time. Scale is
            // never simulated, so that still belongs to the scene.
            mask &= script::ScriptHost::XformScale;
        }
        if (h && mask)
            h->SetObjectTransform(id, pos, rot, scale, mask);
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
        float pos[3], rot[3], scale[3];
        readTransform(L, id, pos, rot, scale); // seed scale (and rot, if omitted)
        pos[0] = (float)luaL_optnumber(L, 2, 0);
        pos[1] = (float)luaL_optnumber(L, 3, 0);
        pos[2] = (float)luaL_optnumber(L, 4, 0);
        rot[0] = (float)luaL_optnumber(L, 5, 0);
        rot[1] = (float)luaL_optnumber(L, 6, 0);
        rot[2] = (float)luaL_optnumber(L, 7, 0);
        writeTransform(L, id, pos, rot, scale,
                       script::ScriptHost::XformPos | script::ScriptHost::XformRot);
        return 0;
    }
    int obj_set_position(lua_State* L)
    {
        const int id = getSelfId(L, 1);
        float pos[3], rot[3], scale[3];
        readTransform(L, id, pos, rot, scale);
        pos[0] = (float)luaL_optnumber(L, 2, pos[0]);
        pos[1] = (float)luaL_optnumber(L, 3, pos[1]);
        pos[2] = (float)luaL_optnumber(L, 4, pos[2]);
        writeTransform(L, id, pos, rot, scale, script::ScriptHost::XformPos);
        return 0;
    }
    int obj_position(lua_State* L)
    {
        float pos[3], rot[3], scale[3];
        readTransform(L, getSelfId(L, 1), pos, rot, scale);
        lua_pushnumber(L, pos[0]); lua_pushnumber(L, pos[1]); lua_pushnumber(L, pos[2]);
        return 3;
    }
    int obj_scale(lua_State* L)
    {
        float pos[3], rot[3], scale[3];
        readTransform(L, getSelfId(L, 1), pos, rot, scale);
        lua_pushnumber(L, scale[0]); lua_pushnumber(L, scale[1]); lua_pushnumber(L, scale[2]);
        return 3;
    }
    int obj_set_scale(lua_State* L)
    {
        const int id = getSelfId(L, 1);
        float pos[3], rot[3], scale[3];
        readTransform(L, id, pos, rot, scale);
        // One argument scales uniformly: obj:set_scale(2) is the common case.
        scale[0] = (float)luaL_optnumber(L, 2, scale[0]);
        scale[1] = (float)luaL_optnumber(L, 3, scale[0]);
        scale[2] = (float)luaL_optnumber(L, 4, scale[0]);
        writeTransform(L, id, pos, rot, scale, script::ScriptHost::XformScale);
        return 0;
    }
    int obj_visible(lua_State* L)
    {
        script::ScriptHost* h = getHost(L);
        lua_pushboolean(L, h ? (h->GetObjectVisible(getSelfId(L, 1)) ? 1 : 0) : 1);
        return 1;
    }
    int obj_set_visible(lua_State* L)
    {
        script::ScriptHost* h = getHost(L);
        if (h) h->SetObjectVisible(getSelfId(L, 1), lua_toboolean(L, 2) != 0);
        return 0;
    }
    int obj_show(lua_State* L)
    {
        script::ScriptHost* h = getHost(L);
        if (h) h->SetObjectVisible(getSelfId(L, 1), true);
        return 0;
    }
    int obj_hide(lua_State* L)
    {
        script::ScriptHost* h = getHost(L);
        if (h) h->SetObjectVisible(getSelfId(L, 1), false);
        return 0;
    }
    // --- attribute fields: obj:get(field [, attrIndex]) / obj:set(field, ...) ---
    // Two functions cover every attribute field, because the shared
    // scene/AttrFields.h table already knows each field's name and type.
    int obj_get(lua_State* L)
    {
        script::ScriptHost* h = getHost(L);
        const char* field = luaL_checkstring(L, 2);
        // Lua indices are 1-based; -1 (the default) means "search the object".
        const int attrIndex = (int)luaL_optinteger(L, 3, 0) - 1;

        attrfields::Value v;
        if (!h || !h->AttrGet(getSelfId(L, 1), attrIndex, field, v))
        {
            lua_pushnil(L);
            return 1;
        }
        switch (v.kind)
        {
        case attrfields::KindFloat: lua_pushnumber(L, v.f[0]); return 1;
        case attrfields::KindInt:   lua_pushinteger(L, v.i);   return 1;
        case attrfields::KindBool:  lua_pushboolean(L, v.b[0] ? 1 : 0); return 1;
        case attrfields::KindStr:   lua_pushlstring(L, v.s.data(), v.s.size()); return 1;
        case attrfields::KindVec3:
            lua_pushnumber(L, v.f[0]); lua_pushnumber(L, v.f[1]); lua_pushnumber(L, v.f[2]);
            return 3;
        case attrfields::KindBool3:
            lua_pushboolean(L, v.b[0] ? 1 : 0);
            lua_pushboolean(L, v.b[1] ? 1 : 0);
            lua_pushboolean(L, v.b[2] ? 1 : 0);
            return 3;
        }
        lua_pushnil(L);
        return 1;
    }
    int obj_set(lua_State* L)
    {
        script::ScriptHost* h = getHost(L);
        const int id = getSelfId(L, 1);
        const char* field = luaL_checkstring(L, 2);
        const attrfields::FieldDesc* desc = attrfields::Find(field);
        if (!h || !desc) { lua_pushboolean(L, 0); return 1; }

        // The value occupies args 3..n; an optional attribute index follows it.
        // Vec3/Bool3 accept either three components or a single one applied to
        // all three, so obj:set("light_color", 1) is a legal white.
        attrfields::Value v;
        v.kind = desc->kind;
        int used = 1;
        switch (desc->kind)
        {
        case attrfields::KindFloat: v.f[0] = (float)luaL_checknumber(L, 3); break;
        case attrfields::KindInt:   v.i    = (int)luaL_checkinteger(L, 3);  break;
        case attrfields::KindBool:  v.b[0] = lua_toboolean(L, 3) != 0;      break;
        case attrfields::KindStr:   v.s    = luaL_checkstring(L, 3);        break;
        case attrfields::KindVec3:
            v.f[0] = (float)luaL_checknumber(L, 3);
            if (lua_isnumber(L, 4) && lua_isnumber(L, 5))
            {
                v.f[1] = (float)lua_tonumber(L, 4);
                v.f[2] = (float)lua_tonumber(L, 5);
                used = 3;
            }
            else v.f[1] = v.f[2] = v.f[0];
            break;
        case attrfields::KindBool3:
            v.b[0] = lua_toboolean(L, 3) != 0;
            if (lua_isboolean(L, 4) && lua_isboolean(L, 5))
            {
                v.b[1] = lua_toboolean(L, 4) != 0;
                v.b[2] = lua_toboolean(L, 5) != 0;
                used = 3;
            }
            else v.b[1] = v.b[2] = v.b[0];
            break;
        default: lua_pushboolean(L, 0); return 1;
        }

        const int attrIndex = (int)luaL_optinteger(L, 3 + used, 0) - 1;
        lua_pushboolean(L, h->AttrSet(id, attrIndex, field, v) ? 1 : 0);
        return 1;
    }
    int obj_attr_count(lua_State* L)
    {
        script::ScriptHost* h = getHost(L);
        lua_pushinteger(L, h ? h->AttrCount(getSelfId(L, 1)) : 0);
        return 1;
    }
    int obj_attr_type(lua_State* L)
    {
        script::ScriptHost* h = getHost(L);
        const int attrIndex = (int)luaL_checkinteger(L, 2) - 1;
        const char* t = h ? h->AttrType(getSelfId(L, 1), attrIndex) : "";
        lua_pushstring(L, t ? t : "");
        return 1;
    }

    int obj_tags(lua_State* L)
    {
        script::ScriptHost* h = getHost(L);
        const int id = getSelfId(L, 1);
        const int n = h ? h->ObjectTagCount(id) : 0;
        lua_createtable(L, n > 0 ? n : 0, 0);
        for (int i = 0; i < n; ++i)
        {
            const char* t = h->ObjectTag(id, i);
            lua_pushstring(L, t ? t : "");
            lua_seti(L, -2, i + 1);
        }
        return 1;
    }
    int obj_has_tag(lua_State* L)
    {
        script::ScriptHost* h = getHost(L);
        const int id = getSelfId(L, 1);
        const char* want = luaL_checkstring(L, 2);
        const int n = h ? h->ObjectTagCount(id) : 0;
        for (int i = 0; i < n; ++i)
        {
            const char* t = h->ObjectTag(id, i);
            if (t && strcmp(t, want) == 0) { lua_pushboolean(L, 1); return 1; }
        }
        lua_pushboolean(L, 0);
        return 1;
    }
    int obj_add_tag(lua_State* L)
    {
        script::ScriptHost* h = getHost(L);
        lua_pushboolean(L, h ? (h->AddObjectTag(getSelfId(L, 1), luaL_checkstring(L, 2)) ? 1 : 0) : 0);
        return 1;
    }
    int obj_remove_tag(lua_State* L)
    {
        script::ScriptHost* h = getHost(L);
        lua_pushboolean(L, h ? (h->RemoveObjectTag(getSelfId(L, 1), luaL_checkstring(L, 2)) ? 1 : 0) : 0);
        return 1;
    }
    int obj_velocity(lua_State* L)
    {
        phys::PhysicsWorld* p = getPhys(L);
        float v[3] = { 0, 0, 0 };
        if (p) p->GetLinearVelocity(getSelfId(L, 1), v);
        lua_pushnumber(L, v[0]); lua_pushnumber(L, v[1]); lua_pushnumber(L, v[2]);
        return 3;
    }
    int obj_apply_force(lua_State* L)
    {
        phys::PhysicsWorld* p = getPhys(L);
        if (p) p->ApplyForce(getSelfId(L, 1),
                             (float)luaL_optnumber(L, 2, 0),
                             (float)luaL_optnumber(L, 3, 0),
                             (float)luaL_optnumber(L, 4, 0));
        return 0;
    }
    int obj_apply_torque(lua_State* L)
    {
        phys::PhysicsWorld* p = getPhys(L);
        if (p) p->ApplyTorque(getSelfId(L, 1),
                              (float)luaL_optnumber(L, 2, 0),
                              (float)luaL_optnumber(L, 3, 0),
                              (float)luaL_optnumber(L, 4, 0));
        return 0;
    }
    int obj_set_angular_velocity(lua_State* L)
    {
        phys::PhysicsWorld* p = getPhys(L);
        if (p) p->SetAngularVelocity(getSelfId(L, 1),
                                     (float)luaL_optnumber(L, 2, 0),
                                     (float)luaL_optnumber(L, 3, 0),
                                     (float)luaL_optnumber(L, 4, 0));
        return 0;
    }
    int obj_angular_velocity(lua_State* L)
    {
        phys::PhysicsWorld* p = getPhys(L);
        float v[3] = { 0, 0, 0 };
        if (p) p->GetAngularVelocity(getSelfId(L, 1), v);
        lua_pushnumber(L, v[0]); lua_pushnumber(L, v[1]); lua_pushnumber(L, v[2]);
        return 3;
    }
    int obj_rotation(lua_State* L)
    {
        float pos[3], rot[3], scale[3];
        readTransform(L, getSelfId(L, 1), pos, rot, scale);
        lua_pushnumber(L, rot[0]); lua_pushnumber(L, rot[1]); lua_pushnumber(L, rot[2]);
        return 3;
    }
    int obj_set_rotation(lua_State* L)
    {
        // Rotation-only: keep the object where it is, re-aim it.
        const int id = getSelfId(L, 1);
        float pos[3], rot[3], scale[3];
        readTransform(L, id, pos, rot, scale);
        rot[0] = (float)luaL_optnumber(L, 2, rot[0]);
        rot[1] = (float)luaL_optnumber(L, 3, rot[1]);
        rot[2] = (float)luaL_optnumber(L, 4, rot[2]);
        writeTransform(L, id, pos, rot, scale, script::ScriptHost::XformRot);
        return 0;
    }
    int obj_id(lua_State* L)
    {
        // The raw slot, even for a dead handle — it is an identity for logging
        // and table keys, not a capability.
        lua_pushinteger(L, getRawId(L, 1));
        return 1;
    }
    int obj_alive(lua_State* L)
    {
        lua_pushboolean(L, handleAlive(L, 1, getRawId(L, 1)) ? 1 : 0);
        return 1;
    }
    int obj_destroy(lua_State* L)
    {
        script::ScriptHost* h = getHost(L);
        const int id = getSelfId(L, 1);
        lua_pushboolean(L, (h && id >= 0 && h->DestroyObject(id)) ? 1 : 0);
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
    int l_input_pressed(lua_State* L)
    {
        script::ScriptHost* h = getHost(L);
        lua_pushboolean(L, h ? (h->InputButtonPressed(luaL_checkstring(L, 1)) ? 1 : 0) : 0);
        return 1;
    }
    int l_input_released(lua_State* L)
    {
        script::ScriptHost* h = getHost(L);
        lua_pushboolean(L, h ? (h->InputButtonReleased(luaL_checkstring(L, 1)) ? 1 : 0) : 0);
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

    // Enumeration skips destroyed slots — a freed slot is not an object until a
    // spawn reuses it.
    int l_find_all(lua_State* L)
    {
        script::ScriptHost* h = getHost(L);
        const int n = h ? h->ObjectCount() : 0;
        lua_newtable(L);
        int count = 0;
        for (int i = 0; i < n; ++i)
        {
            if (!h->ObjectAlive(i)) continue;
            pushObject(L, i);
            lua_seti(L, -2, ++count); // Lua arrays are 1-based
        }
        return 1;
    }
    int l_find_by_prefix(lua_State* L)
    {
        script::ScriptHost* h = getHost(L);
        const char* prefix = luaL_checkstring(L, 1);
        const size_t plen = strlen(prefix);
        const int n = h ? h->ObjectCount() : 0;
        lua_newtable(L);
        int count = 0;
        for (int i = 0; i < n; ++i)
        {
            if (!h->ObjectAlive(i)) continue;
            const char* name = h->ObjectName(i);
            if (!name || strncmp(name, prefix, plen) != 0) continue;
            pushObject(L, i);
            lua_seti(L, -2, ++count);
        }
        return 1;
    }

    int l_find_by_tag(lua_State* L)
    {
        script::ScriptHost* h = getHost(L);
        const char* want = luaL_checkstring(L, 1);
        const int n = h ? h->ObjectCount() : 0;
        lua_newtable(L);
        int count = 0;
        for (int i = 0; i < n; ++i)
        {
            if (!h->ObjectAlive(i)) continue;
            const int tags = h->ObjectTagCount(i);
            for (int t = 0; t < tags; ++t)
            {
                const char* tag = h->ObjectTag(i, t);
                if (!tag || strcmp(tag, want) != 0) continue;
                pushObject(L, i);
                lua_seti(L, -2, ++count);
                break;
            }
        }
        return 1;
    }

    // spawn(source, name, x, y, z) -> handle | nil
    // `source` is a template object: a handle, or a name. The clone inherits its
    // attributes, rotation and scale, so an authored (usually hidden) template
    // is the unit of spawning — and its assets are already in the pak.
    int l_spawn(lua_State* L)
    {
        script::ScriptHost* h = getHost(L);
        if (!h) { lua_pushnil(L); return 1; }

        int source = -1;
        if (lua_istable(L, 1))       source = getSelfId(L, 1);
        else if (lua_isstring(L, 1)) source = h->FindObject(lua_tostring(L, 1));
        if (source < 0) { lua_pushnil(L); return 1; }

        const char* name = luaL_optstring(L, 2, "");
        float pos[3];
        // Default to the template's own position, so spawn(t, "x") is a copy.
        float srcRot[3], srcScale[3];
        if (!h->GetObjectTransform(source, pos, srcRot, srcScale))
            pos[0] = pos[1] = pos[2] = 0.0f;
        pos[0] = (float)luaL_optnumber(L, 3, pos[0]);
        pos[1] = (float)luaL_optnumber(L, 4, pos[1]);
        pos[2] = (float)luaL_optnumber(L, 5, pos[2]);

        const int spawned = h->SpawnObject(source, name, pos);
        if (spawned < 0) lua_pushnil(L);
        else             pushObject(L, spawned);
        return 1;
    }

    // --- scene table: switching levels ---
    int l_scene_load(lua_State* L)
    {
        script::ScriptHost* h = getHost(L);
        // No artificial hold by default: the hand-over happens the moment the
        // meshes and their small mips are in. Opt in only if a screen is wanted
        // for longer than the load justifies.
        const float minSeconds = (float)luaL_optnumber(L, 2, 0.0);
        lua_pushboolean(L, (h && h->LoadScene(luaL_checkstring(L, 1), minSeconds)) ? 1 : 0);
        return 1;
    }
    int l_scene_is_loading(lua_State* L)
    {
        script::ScriptHost* h = getHost(L);
        lua_pushboolean(L, (h && h->SceneIsLoading()) ? 1 : 0);
        return 1;
    }
    int l_scene_progress(lua_State* L)
    {
        script::ScriptHost* h = getHost(L);
        lua_pushnumber(L, h ? h->SceneProgress() : 1.0);
        return 1;
    }
    int l_scene_name(lua_State* L)
    {
        script::ScriptHost* h = getHost(L);
        const char* n = h ? h->SceneName() : "";
        lua_pushstring(L, n ? n : "");
        return 1;
    }

    // --- camera table: which "Camera" attribute drives the view ---
    int l_camera_set_active(lua_State* L)
    {
        script::ScriptHost* h = getHost(L);
        int id = -1;
        if (lua_istable(L, 1))       id = getSelfId(L, 1);
        else if (lua_isstring(L, 1)) id = h ? h->FindObject(lua_tostring(L, 1)) : -1;
        else                         id = (int)luaL_optinteger(L, 1, -1);
        lua_pushboolean(L, (h && id >= 0 && h->SetActiveCamera(id)) ? 1 : 0);
        return 1;
    }
    int l_camera_active(lua_State* L)
    {
        script::ScriptHost* h = getHost(L);
        const int id = h ? h->GetActiveCamera() : -1;
        if (id < 0) lua_pushnil(L);
        else        pushObject(L, id);
        return 1;
    }

    // --- physics table: world queries that aren't about one object ---
    int l_physics_raycast(lua_State* L)
    {
        phys::PhysicsWorld* p = getPhys(L);
        if (!p) { lua_pushnil(L); return 1; }
        const float origin[3] = { (float)luaL_checknumber(L, 1),
                                  (float)luaL_checknumber(L, 2),
                                  (float)luaL_checknumber(L, 3) };
        const float dir[3]    = { (float)luaL_checknumber(L, 4),
                                  (float)luaL_checknumber(L, 5),
                                  (float)luaL_checknumber(L, 6) };
        const float maxDist   = (float)luaL_optnumber(L, 7, 1000.0);

        phys::RayHit hit;
        if (!p->Raycast(origin, dir, maxDist, hit)) { lua_pushnil(L); return 1; }

        lua_createtable(L, 0, 8);
        if (hit.objectIndex >= 0) pushObject(L, hit.objectIndex);
        else                      lua_pushnil(L);
        lua_setfield(L, -2, "object");
        lua_pushnumber(L, hit.distance);  lua_setfield(L, -2, "distance");
        lua_pushnumber(L, hit.point[0]);  lua_setfield(L, -2, "x");
        lua_pushnumber(L, hit.point[1]);  lua_setfield(L, -2, "y");
        lua_pushnumber(L, hit.point[2]);  lua_setfield(L, -2, "z");
        lua_pushnumber(L, hit.normal[0]); lua_setfield(L, -2, "nx");
        lua_pushnumber(L, hit.normal[1]); lua_setfield(L, -2, "ny");
        lua_pushnumber(L, hit.normal[2]); lua_setfield(L, -2, "nz");
        return 1;
    }

    // --- time table: time.delta / time.total are plain fields refreshed each
    // frame by ScriptVM::Update; these three are the scheduler. ---
    int timerAdd(lua_State* L, bool repeating)
    {
        script::VmState* vm = getVm(L);
        const double seconds = (double)luaL_checknumber(L, 1);
        luaL_checktype(L, 2, LUA_TFUNCTION);
        if (!vm) { lua_pushnil(L); return 1; }

        script::VmState::Timer t;
        t.id        = vm->nextTimerId++;
        t.due       = vm->now + (seconds > 0.0 ? seconds : 0.0);
        t.interval  = repeating ? (seconds > 0.0 ? seconds : 0.0) : 0.0;
        t.repeating = repeating;
        lua_pushvalue(L, 2);
        t.ref = luaL_ref(L, LUA_REGISTRYINDEX); // anchors the callback
        vm->timers.push_back(t);

        lua_pushinteger(L, t.id);
        return 1;
    }
    int l_time_after(lua_State* L) { return timerAdd(L, false); }
    int l_time_every(lua_State* L) { return timerAdd(L, true); }
    int l_time_cancel(lua_State* L)
    {
        script::VmState* vm = getVm(L);
        const int id = (int)luaL_checkinteger(L, 1);
        if (vm)
        {
            for (size_t i = 0; i < vm->timers.size(); ++i)
            {
                if (vm->timers[i].id != id || vm->timers[i].dead) continue;
                vm->timers[i].dead = true;
                // Released here rather than in the sweep so a cancel inside a
                // callback can't leave the ref alive for a frame.
                luaL_unref(L, LUA_REGISTRYINDEX, vm->timers[i].ref);
                vm->timers[i].ref = LUA_NOREF;
                lua_pushboolean(L, 1);
                return 1;
            }
        }
        lua_pushboolean(L, 0);
        return 1;
    }

    // --- event table: string-keyed pub/sub between scripts ---
    int l_event_on(lua_State* L)
    {
        const char* name = luaL_checkstring(L, 1);
        luaL_checktype(L, 2, LUA_TFUNCTION);
        lua_getfield(L, LUA_REGISTRYINDEX, "xf_events");  // [events]
        lua_getfield(L, -1, name);                        // [events, list|nil]
        if (!lua_istable(L, -1))
        {
            lua_pop(L, 1);
            lua_newtable(L);                              // [events, list]
            lua_pushvalue(L, -1);                         // [events, list, list]
            lua_setfield(L, -3, name);                    // events[name] = list
        }
        const lua_Integer n = (lua_Integer)lua_rawlen(L, -1);
        lua_pushvalue(L, 2);                              // [events, list, fn]
        lua_seti(L, -2, n + 1);
        lua_pop(L, 2);
        return 0;
    }
    int l_event_emit(lua_State* L)
    {
        script::ScriptHost* h = getHost(L);
        const char* name = luaL_checkstring(L, 1);
        lua_getfield(L, LUA_REGISTRYINDEX, "xf_events");  // [events]
        lua_getfield(L, -1, name);                        // [events, list|nil]
        if (!lua_istable(L, -1)) { lua_pop(L, 2); return 0; }

        // Snapshot the count: a handler that subscribes during dispatch must not
        // be called by this same emit (and must not shift what we iterate).
        const lua_Integer count = (lua_Integer)lua_rawlen(L, -1);
        for (lua_Integer i = 1; i <= count; ++i)
        {
            lua_geti(L, -1, i);                           // [events, list, fn]
            if (!lua_isfunction(L, -1)) { lua_pop(L, 1); continue; }
            lua_pushvalue(L, 2);                          // payload (may be none -> nil)
            if (lua_pcall(L, 1, 0, 0) != LUA_OK)
            {
                // One bad handler must not stop the rest.
                if (h) h->LogError((std::string("event '") + name + "': " +
                                    lua_tostring(L, -1)).c_str());
                lua_pop(L, 1);
            }
        }
        lua_pop(L, 2);
        return 0;
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
    // Animated-GIF play mode. Spelled as a string here to match the
    // neighbouring gui options (align/valign/anchor); the Image attribute's
    // image_play_mode is the same three values as an int, which is that layer's
    // own convention for enum fields.
    int playModeFromName(const char* name, int fallback)
    {
        if (!name) return fallback;
        if (!strcmp(name, "off"))  return gifanim::PlayOff;
        if (!strcmp(name, "once")) return gifanim::PlayOnce;
        if (!strcmp(name, "loop")) return gifanim::PlayLoop;
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

        lua_getfield(L, optsIdx, "play_mode");
        if (lua_isstring(L, -1))
            g->SetPlayMode(h, playModeFromName(lua_tostring(L, -1), gifanim::PlayLoop));
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

    int l_gui_set_play_mode(lua_State* L)
    {
        gui::Context* g = getGui(L);
        if (g) g->SetPlayMode(toWidget(L, 1),
                              playModeFromName(lua_tostring(L, 2), gifanim::PlayLoop));
        return 0;
    }
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
        // lua_close frees the registry, so every timer's callback ref goes with
        // it — the scheduler only has to forget its own bookkeeping.
        if (m_impl->L) { lua_close(m_impl->L); m_impl->L = 0; }
        m_impl->phys = 0;
        m_impl->host = 0;
        m_impl->gui  = 0;
        m_impl->scripted.clear();
        m_impl->vm.timers.clear();
        m_impl->vm.nextTimerId = 1;
        m_impl->vm.now = 0.0;
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
        lua_pushlightuserdata(L, (void*)&m_impl->vm); lua_setfield(L, LUA_REGISTRYINDEX, "xf_vm");

        // Object handle metatable: __index -> a table of the object methods.
        luaL_newmetatable(L, "xf_object");
        lua_newtable(L);
        static const luaL_Reg objMethods[] = {
            { "apply_impulse", obj_apply_impulse },
            { "apply_force",   obj_apply_force },
            { "apply_torque",  obj_apply_torque },
            { "set_velocity",  obj_set_velocity },
            { "set_angular_velocity", obj_set_angular_velocity },
            { "angular_velocity",     obj_angular_velocity },
            { "set_transform", obj_set_transform },
            { "set_position",  obj_set_position },
            { "set_rotation",  obj_set_rotation },
            { "set_scale",     obj_set_scale },
            { "position",      obj_position },
            { "rotation",      obj_rotation },
            { "scale",         obj_scale },
            { "velocity",      obj_velocity },
            { "visible",       obj_visible },
            { "set_visible",   obj_set_visible },
            { "show",          obj_show },
            { "hide",          obj_hide },
            { "get",           obj_get },
            { "set",           obj_set },
            { "attr_count",    obj_attr_count },
            { "attr_type",     obj_attr_type },
            { "tags",          obj_tags },
            { "has_tag",       obj_has_tag },
            { "add_tag",       obj_add_tag },
            { "remove_tag",    obj_remove_tag },
            { "destroy",       obj_destroy },
            { "alive",         obj_alive },
            { "id",            obj_id },
            { "name",          obj_name },
            { NULL, NULL }
        };
        luaL_setfuncs(L, objMethods, 0);
        lua_setfield(L, -2, "__index");
        lua_pop(L, 1); // metatable

        // Global input table + log/find.
        lua_newtable(L);
        lua_pushcfunction(L, l_input_button);   lua_setfield(L, -2, "button");
        lua_pushcfunction(L, l_input_pressed);  lua_setfield(L, -2, "pressed");
        lua_pushcfunction(L, l_input_released); lua_setfield(L, -2, "released");
        lua_pushcfunction(L, l_input_axis);     lua_setfield(L, -2, "axis");
        lua_setglobal(L, "input");
        lua_pushcfunction(L, l_log);  lua_setglobal(L, "log");
        lua_pushcfunction(L, l_find); lua_setglobal(L, "find");
        lua_pushcfunction(L, l_find_all);       lua_setglobal(L, "find_all");
        lua_pushcfunction(L, l_find_by_prefix); lua_setglobal(L, "find_by_prefix");
        lua_pushcfunction(L, l_find_by_tag);    lua_setglobal(L, "find_by_tag");
        lua_pushcfunction(L, l_spawn);          lua_setglobal(L, "spawn");

        // Global scene table (level switching).
        lua_newtable(L);
        lua_pushcfunction(L, l_scene_load);       lua_setfield(L, -2, "load");
        lua_pushcfunction(L, l_scene_is_loading); lua_setfield(L, -2, "is_loading");
        lua_pushcfunction(L, l_scene_progress);   lua_setfield(L, -2, "progress");
        lua_pushcfunction(L, l_scene_name);       lua_setfield(L, -2, "name");
        lua_setglobal(L, "scene");

        // Global physics table (world queries).
        lua_newtable(L);
        lua_pushcfunction(L, l_physics_raycast); lua_setfield(L, -2, "raycast");
        lua_setglobal(L, "physics");

        // Global camera table (which Camera attribute drives the view).
        lua_newtable(L);
        lua_pushcfunction(L, l_camera_set_active); lua_setfield(L, -2, "set_active");
        lua_pushcfunction(L, l_camera_active);     lua_setfield(L, -2, "active");
        lua_setglobal(L, "camera");

        // Global time table. `delta`/`total` are seeded here and refreshed by
        // Update; after/every/cancel drive the scheduler in VmState.
        lua_newtable(L);
        lua_pushnumber(L, 0.0); lua_setfield(L, -2, "delta");
        lua_pushnumber(L, 0.0); lua_setfield(L, -2, "total");
        lua_pushcfunction(L, l_time_after);  lua_setfield(L, -2, "after");
        lua_pushcfunction(L, l_time_every);  lua_setfield(L, -2, "every");
        lua_pushcfunction(L, l_time_cancel); lua_setfield(L, -2, "cancel");
        lua_setglobal(L, "time");

        // Global event table + its registry backing (eventName -> {fn, ...}).
        lua_newtable(L);
        lua_pushcfunction(L, l_event_on);   lua_setfield(L, -2, "on");
        lua_pushcfunction(L, l_event_emit); lua_setfield(L, -2, "emit");
        lua_setglobal(L, "event");
        lua_newtable(L);
        lua_setfield(L, LUA_REGISTRYINDEX, "xf_events");

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
            lua_pushcfunction(L, l_gui_set_play_mode); lua_setfield(L, -2, "set_play_mode");
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

    void ScriptVM::StartOne(int objectIndex)
    {
        lua_State* L = m_impl->L;
        if (!L) return;
        if (!beginCall(L, objectIndex, "on_start")) return;
        if (lua_pcall(L, 0, 0, 0) != LUA_OK)
        {
            if (m_impl->host) m_impl->host->LogError((std::string("on_start: ") + lua_tostring(L, -1)).c_str());
            lua_pop(L, 1);
        }
        lua_pop(L, 2); // env + scripts
    }

    void ScriptVM::Start()
    {
        lua_State* L = m_impl->L;
        if (!L) return;
        for (size_t i = 0; i < m_impl->scripted.size(); ++i)
            StartOne(m_impl->scripted[i]);
    }

    void ScriptVM::UnloadScript(int objectIndex)
    {
        lua_State* L = m_impl->L;
        if (!L) return;

        // on_destroy runs while the object's environment is still intact, so the
        // handler can still read self and talk to the rest of the scene.
        if (beginCall(L, objectIndex, "on_destroy"))
        {
            if (lua_pcall(L, 0, 0, 0) != LUA_OK)
            {
                if (m_impl->host)
                    m_impl->host->LogError((std::string("on_destroy: ") + lua_tostring(L, -1)).c_str());
                lua_pop(L, 1);
            }
            lua_pop(L, 2); // env + scripts
        }

        // Drop the environment: without this a reused slot would inherit the
        // previous object's on_update.
        lua_getfield(L, LUA_REGISTRYINDEX, "xf_scripts");
        lua_pushnil(L);
        lua_seti(L, -2, objectIndex);
        lua_pop(L, 1);

        for (size_t i = 0; i < m_impl->scripted.size(); ++i)
            if (m_impl->scripted[i] == objectIndex)
            { m_impl->scripted.erase(m_impl->scripted.begin() + i); break; }
    }

    // Fire every timer that came due, then re-arm the repeating ones. Runs
    // before on_update so a callback's effects are visible the same frame.
    void ScriptVM::TickTimers(float dt)
    {
        lua_State* L = m_impl->L;
        VmState&   vm = m_impl->vm;
        vm.now += (double)dt;

        // Collect first: a callback is free to add or cancel timers, which would
        // otherwise reallocate the vector out from under this loop.
        std::vector<int> due;
        for (size_t i = 0; i < vm.timers.size(); ++i)
        {
            if (vm.timers[i].dead || vm.timers[i].due > vm.now) continue;
            due.push_back(vm.timers[i].id);
        }

        for (size_t d = 0; d < due.size(); ++d)
        {
            // Re-find by id — the vector may have moved since we collected.
            size_t slot = vm.timers.size();
            for (size_t i = 0; i < vm.timers.size(); ++i)
                if (vm.timers[i].id == due[d]) { slot = i; break; }
            if (slot == vm.timers.size() || vm.timers[slot].dead) continue;

            const int ref = vm.timers[slot].ref;
            if (vm.timers[slot].repeating)
            {
                // Re-arm from the deadline so a steady interval doesn't drift,
                // but never let a long frame queue up a burst of catch-up calls.
                const double interval = vm.timers[slot].interval > 0.0
                                            ? vm.timers[slot].interval : 0.0;
                vm.timers[slot].due += interval;
                if (interval <= 0.0 || vm.timers[slot].due <= vm.now)
                    vm.timers[slot].due = vm.now + interval;
            }
            else
            {
                vm.timers[slot].dead = true;
                vm.timers[slot].ref  = LUA_NOREF;
            }

            lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
            if (lua_isfunction(L, -1))
            {
                if (lua_pcall(L, 0, 0, 0) != LUA_OK)
                {
                    if (m_impl->host)
                        m_impl->host->LogError((std::string("timer: ") + lua_tostring(L, -1)).c_str());
                    lua_pop(L, 1);
                }
            }
            else lua_pop(L, 1);

            // A one-shot's ref is only released after the call, so the callback
            // itself stays alive while it runs.
            if (!vm.timers[slot].repeating)
                luaL_unref(L, LUA_REGISTRYINDEX, ref);
        }

        // Sweep the dead so a long session doesn't grow the list forever.
        size_t keep = 0;
        for (size_t i = 0; i < vm.timers.size(); ++i)
            if (!vm.timers[i].dead) vm.timers[keep++] = vm.timers[i];
        vm.timers.resize(keep);
    }

    void ScriptVM::Update(float dt)
    {
        lua_State* L = m_impl->L;
        if (!L) return;

        // Refresh the `time` fields before anything runs this frame.
        lua_getglobal(L, "time");
        if (lua_istable(L, -1))
        {
            lua_pushnumber(L, (lua_Number)dt);          lua_setfield(L, -2, "delta");
            lua_pushnumber(L, m_impl->vm.now + (double)dt); lua_setfield(L, -2, "total");
        }
        lua_pop(L, 1);

        TickTimers(dt);

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

    void ScriptVM::FireTrigger(int objectIndex, int otherObjectIndex,
                               const char* boneName, int phase)
    {
        lua_State* L = m_impl->L;
        if (!L) return;
        const char* handler = (phase == phys::PhaseStay) ? "on_trigger_stay"
                            : (phase == phys::PhaseExit) ? "on_trigger_exit"
                                                         : "on_trigger";
        if (!beginCall(L, objectIndex, handler)) return;
        pushObject(L, otherObjectIndex);
        if (boneName && boneName[0]) lua_pushstring(L, boneName);
        else lua_pushnil(L);
        if (lua_pcall(L, 2, 0, 0) != LUA_OK)
        {
            if (m_impl->host) m_impl->host->LogError((std::string(handler) + ": " + lua_tostring(L, -1)).c_str());
            lua_pop(L, 1);
        }
        lua_pop(L, 2); // env + scripts
    }

    void ScriptVM::FireCollision(int aObjectIndex, int bObjectIndex, int phase)
    {
        lua_State* L = m_impl->L;
        if (!L) return;
        const char* phaseName = (phase == phys::PhaseStay) ? "stay"
                              : (phase == phys::PhaseExit) ? "exit"
                                                           : "enter";
        // Both sides hear about it, each seeing the other as `other`.
        const int pairs[2][2] = { { aObjectIndex, bObjectIndex },
                                  { bObjectIndex, aObjectIndex } };
        for (int i = 0; i < 2; ++i)
        {
            if (!beginCall(L, pairs[i][0], "on_collision")) continue;
            pushObject(L, pairs[i][1]);
            lua_pushstring(L, phaseName);
            if (lua_pcall(L, 2, 0, 0) != LUA_OK)
            {
                if (m_impl->host) m_impl->host->LogError((std::string("on_collision: ") + lua_tostring(L, -1)).c_str());
                lua_pop(L, 1);
            }
            lua_pop(L, 2); // env + scripts
        }
    }
}
