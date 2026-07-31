#include "script/ScriptVM.h"
#include "script/ScriptTypes.h"
#include "physics/PhysicsWorld.h"

extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}

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
        std::vector<int>    scripted; // objectIndices with a loaded script

        Impl() : L(0), phys(0), host(0) {}
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
        m_impl->scripted.clear();
    }

    void ScriptVM::Begin(phys::PhysicsWorld* phys, ScriptHost* host)
    {
        Clear();
        m_impl->phys = phys;
        m_impl->host = host;
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
