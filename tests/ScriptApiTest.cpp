#include "script/ScriptVM.h"
#include "script/ScriptTypes.h"
#include "physics/PhysicsWorld.h"
#include "scene/AttrFields.h"
#include "state/EngineState.h"

#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

// Drives the shared ScriptVM through a stub host — no renderer, no device — to
// check the Lua surface itself: the time scheduler, the event bus, the physics
// table, scene enumeration, and the trigger/collision handler dispatch.

namespace
{
    bool Near(float left, float right, float tol = 1.0e-3f)
    {
        return (left > right ? left - right : right - left) < tol;
    }

    // Stands in for SceneRenderer's m_live / SceneRuntime's m_scene.
    struct StubObject
    {
        float pos[3];
        float rot[3];
        float scale[3];
        bool  visible;
        bool  isCamera;
        bool  camActive;
        std::vector<std::string> tags;
        // The test links the EDITOR's field table (AttrFieldsEditor.cpp), so the
        // attributes it stores must be ObjectAttribute — the same pairing every
        // host must honour between its table and its struct.
        std::vector<ObjectAttribute> attrs;
        StubObject() : visible(true), isCamera(false), camActive(false)
        {
            pos[0] = pos[1] = pos[2] = 0.0f;
            rot[0] = rot[1] = rot[2] = 0.0f;
            scale[0] = scale[1] = scale[2] = 1.0f;
        }
    };

    struct StubHost : public script::ScriptHost
    {
        std::vector<std::string> logs;
        std::vector<std::string> errors;
        std::vector<std::string> names;
        std::vector<StubObject>  objects;

        bool  InputButton(const char*) { return false; }
        float InputAxis(const char*)   { return 0.0f; }
        void  Log(const char* msg)      { logs.push_back(msg ? msg : ""); }
        void  LogError(const char* msg) { errors.push_back(msg ? msg : ""); }

        int FindObject(const char* name)
        {
            for (size_t i = 0; i < names.size(); ++i)
                if (names[i] == name) return (int)i;
            return -1;
        }
        int ObjectCount() { return (int)names.size(); }
        const char* ObjectName(int index)
        {
            if (index >= 0 && index < (int)names.size()) return names[index].c_str();
            return "";
        }

        bool Valid(int i) const { return i >= 0 && i < (int)objects.size(); }

        bool GetObjectTransform(int i, float pos[3], float rot[3], float scale[3])
        {
            if (!Valid(i)) return false;
            for (int k = 0; k < 3; ++k)
            { pos[k] = objects[i].pos[k]; rot[k] = objects[i].rot[k]; scale[k] = objects[i].scale[k]; }
            return true;
        }
        void SetObjectTransform(int i, const float pos[3], const float rot[3],
                                const float scale[3], int mask)
        {
            if (!Valid(i)) return;
            if (mask & XformPos)   for (int k = 0; k < 3; ++k) objects[i].pos[k]   = pos[k];
            if (mask & XformRot)   for (int k = 0; k < 3; ++k) objects[i].rot[k]   = rot[k];
            if (mask & XformScale) for (int k = 0; k < 3; ++k) objects[i].scale[k] = scale[k];
        }
        bool GetObjectVisible(int i) { return Valid(i) ? objects[i].visible : false; }
        void SetObjectVisible(int i, bool v) { if (Valid(i)) objects[i].visible = v; }
        bool SetActiveCamera(int i)
        {
            if (!Valid(i) || !objects[i].isCamera) return false;
            for (size_t k = 0; k < objects.size(); ++k)
                if (objects[k].isCamera) objects[k].camActive = ((int)k == i);
            return true;
        }
        int GetActiveCamera()
        {
            for (size_t k = 0; k < objects.size(); ++k)
                if (objects[k].isCamera && objects[k].camActive) return (int)k;
            return -1;
        }
        // Slot lifetime, mirroring both real hosts: append-only slots, a free
        // list, and a generation bumped on every destroy AND every reuse.
        std::vector<unsigned>      gens;
        std::vector<unsigned char> alive;

        void Populate() // call once the names/objects are set up
        {
            gens.assign(names.size(), 1u);
            alive.assign(names.size(), 1);
        }
        // Tests that don't exercise lifetime skip Populate(); they then get the
        // "everything is generation 1 and alive" behaviour of a scene that has
        // never spawned or destroyed anything.
        unsigned ObjectGeneration(int i)
        {
            if (i < 0) return 0u;
            if (i < (int)gens.size()) return gens[i];
            return (i < (int)names.size()) ? 1u : 0u;
        }
        bool ObjectAlive(int i)
        {
            if (i < 0) return false;
            if (i < (int)alive.size()) return alive[i] != 0;
            return i < (int)names.size();
        }

        int SpawnObject(int source, const char* name, const float pos[3])
        {
            if (!Valid(source)) return -1;
            int slot = -1;
            for (size_t i = 0; i < alive.size(); ++i)
                if (!alive[i]) { slot = (int)i; break; }
            if (slot < 0)
            {
                objects.push_back(StubObject());
                names.push_back(std::string());
                gens.push_back(0u);
                alive.push_back(0);
                slot = (int)objects.size() - 1;
            }
            const StubObject src = objects[source];
            objects[slot] = src;
            names[slot] = (name && name[0]) ? name : "";
            for (int k = 0; k < 3; ++k) objects[slot].pos[k] = pos[k];
            objects[slot].visible = true;
            ++gens[slot];
            alive[slot] = 1;
            return slot;
        }
        bool DestroyObject(int i)
        {
            if (!ObjectAlive(i)) return false;
            alive[i] = 0;
            ++gens[i];
            return true;
        }

        // Scene loading, modelled on SceneRuntime's prefetch state machine: the
        // load is begun, held while "assets stream", then handed over.
        std::string sceneName;
        std::string loadingScene;
        float loadElapsed;
        float loadMin;
        float residency;   // the test drives this to stand in for the streamer
        int   handovers;

        bool LoadScene(const char* name, float minSeconds)
        {
            if (!loadingScene.empty()) return false; // one at a time
            loadingScene = name ? name : "";
            if (loadingScene.empty()) return false;
            loadElapsed = 0.0f;
            loadMin = minSeconds > 0.0f ? minSeconds : 0.0f;
            residency = 0.0f;
            return true;
        }
        bool  SceneIsLoading() { return !loadingScene.empty(); }
        // Mirrors SceneRuntime: the fraction of the bytes this load has to read
        // that have arrived. No time term — `residency` is driven by the test.
        float SceneProgress() { return loadingScene.empty() ? 1.0f : residency; }
        const char* SceneName() { return sceneName.c_str(); }

        // Stand-in for SceneRuntime::UpdateSceneLoad's hand-over rule.
        void TickLoad(float dt)
        {
            if (loadingScene.empty()) return;
            loadElapsed += dt;
            if (residency >= 1.0f && loadElapsed >= loadMin)
            {
                sceneName = loadingScene;
                loadingScene.clear();
                ++handovers;
            }
        }

        int AttrCount(int i) { return Valid(i) ? (int)objects[i].attrs.size() : 0; }
        const char* AttrType(int i, int a)
        {
            if (!Valid(i) || a < 0 || a >= (int)objects[i].attrs.size()) return "";
            return objects[i].attrs[a].type.c_str();
        }
        bool AttrGet(int i, int a, const char* field, attrfields::Value& out)
        {
            if (!Valid(i)) return false;
            const attrfields::FieldDesc* d = attrfields::Find(field);
            if (!d) return false;
            const int k = attrfields::ResolveIndex(objects[i].attrs, a, *d);
            if (k < 0) return false;
            return attrfields::Get(&objects[i].attrs[k], *d, out);
        }
        bool AttrSet(int i, int a, const char* field, const attrfields::Value& in)
        {
            if (!Valid(i)) return false;
            const attrfields::FieldDesc* d = attrfields::Find(field);
            if (!d) return false;
            const int k = attrfields::ResolveIndex(objects[i].attrs, a, *d);
            if (k < 0) return false;
            return attrfields::Set(&objects[i].attrs[k], *d, in);
        }

        int ObjectTagCount(int i) { return Valid(i) ? (int)objects[i].tags.size() : 0; }
        const char* ObjectTag(int i, int t)
        {
            if (!Valid(i) || t < 0 || t >= (int)objects[i].tags.size()) return "";
            return objects[i].tags[t].c_str();
        }
        bool AddObjectTag(int i, const char* tag)
        {
            if (!Valid(i) || !tag || !tag[0]) return false;
            for (size_t k = 0; k < objects[i].tags.size(); ++k)
                if (objects[i].tags[k] == tag) return false;
            objects[i].tags.push_back(tag);
            return true;
        }
        bool RemoveObjectTag(int i, const char* tag)
        {
            if (!Valid(i) || !tag) return false;
            for (size_t k = 0; k < objects[i].tags.size(); ++k)
                if (objects[i].tags[k] == tag)
                { objects[i].tags.erase(objects[i].tags.begin() + k); return true; }
            return false;
        }

        bool Logged(const char* text) const
        {
            for (size_t i = 0; i < logs.size(); ++i)
                if (logs[i] == text) return true;
            return false;
        }
        int CountLogged(const char* text) const
        {
            int n = 0;
            for (size_t i = 0; i < logs.size(); ++i)
                if (logs[i] == text) ++n;
            return n;
        }
        StubHost() : loadElapsed(0.0f), loadMin(0.0f), residency(0.0f), handovers(0) {}

        void Dump() const
        {
            for (size_t i = 0; i < logs.size(); ++i)
                std::printf("  log[%d] %s\n", (int)i, logs[i].c_str());
            for (size_t i = 0; i < errors.size(); ++i)
                std::printf("  err[%d] %s\n", (int)i, errors[i].c_str());
        }
    };

    // A floor at the origin so physics.raycast has something to hit, plus a
    // dynamic body on object 0 so the handle's physics verbs are live.
    void BuildWorld(phys::PhysicsWorld& world)
    {
        std::vector<phys::BodyDesc> bodies;

        phys::BodyDesc player;
        player.kind  = phys::BodyDesc::Dynamic;
        player.shape = phys::BodyDesc::Box;
        player.mass  = 1.0f;
        player.pos[1] = 10.0f;
        player.objectIndex = 0;
        bodies.push_back(player);

        phys::BodyDesc wall;
        wall.kind  = phys::BodyDesc::Static;
        wall.shape = phys::BodyDesc::Box;
        wall.halfExtents[0] = wall.halfExtents[1] = wall.halfExtents[2] = 1.0f;
        wall.objectIndex = 1;
        bodies.push_back(wall);

        world.Build(bodies);
    }

    const char* kScript =
        "ticks = 0\n"
        "timer_id = nil\n"
        "function on_start()\n"
        "  log('objects=' .. #find_all())\n"
        "  local walls = find_by_prefix('Wall')\n"
        "  log('walls=' .. #walls .. ' first=' .. walls[1]:name())\n"
        "  local hit = physics.raycast(0, 5, 0, 0, -1, 0, 100)\n"
        "  if hit then\n"
        "    log('ray=' .. hit.object:name() .. ' d=' .. string.format('%.2f', hit.distance)\n"
        "        .. ' ny=' .. string.format('%.2f', hit.ny))\n"
        "  else log('ray=miss') end\n"
        "  if physics.raycast(50, 5, 0, 0, -1, 0, 100) == nil then log('ray2=miss') end\n"
        "  time.after(0.5, function() log('after@' .. string.format('%.2f', time.total)) end)\n"
        "  timer_id = time.every(0.5, function()\n"
        "    ticks = ticks + 1\n"
        "    log('tick' .. ticks)\n"
        "  end)\n"
        "  event.on('ping', function(p) log('h1:' .. tostring(p)) end)\n"
        "  event.on('ping', function(p) error('boom') end)\n"
        "  event.on('ping', function(p) log('h3:' .. tostring(p)) end)\n"
        "  self:set_velocity(1, 0, 0)\n"
        "  self:apply_force(0, 1, 0)\n"
        "  self:apply_torque(0, 1, 0)\n"
        "  self:set_angular_velocity(0, 2, 0)\n"
        "  local ax, ay, az = self:angular_velocity()\n"
        "  log('angvel=' .. string.format('%.1f', ay))\n"
        "end\n"
        "function on_update(dt)\n"
        "  if ticks == 2 and timer_id then\n"
        "    log('cancel=' .. tostring(time.cancel(timer_id)))\n"
        "    timer_id = nil\n"
        "  end\n"
        "  if dt > 0 and time.delta ~= dt then log('DELTA MISMATCH') end\n"
        "end\n"
        "function on_trigger(other, bone) log('enter:' .. other:name()) end\n"
        "function on_trigger_stay(other, bone) log('stay:' .. other:name()) end\n"
        "function on_trigger_exit(other, bone) log('exit:' .. other:name()) end\n"
        "function on_collision(other, phase) log('hit:' .. other:name() .. ':' .. phase) end\n";

    void TestScriptApi()
    {
        StubHost host;
        host.names.push_back("Player");
        host.names.push_back("Wall");
        host.names.push_back("WallB");

        phys::PhysicsWorld world;
        BuildWorld(world);

        script::ScriptVM vm;
        vm.Begin(&world, &host, 0);
        vm.LoadScript(0, kScript, "test.lua");
        // A second script emits on the bus, so subscribe/emit is exercised
        // ACROSS scripts. Load order is dispatch order, so script 0 has already
        // subscribed by the time this one's on_start runs.
        vm.LoadScript(1, "function on_start() event.emit('ping', 42) end\n", "emit.lua");
        assert(!vm.Empty());
        vm.Start();

        // --- on_start: enumeration, raycast, physics verbs ---
        if (!host.Logged("objects=3")) host.Dump();
        assert(host.Logged("objects=3"));
        assert(host.Logged("walls=2 first=Wall"));
        assert(host.Logged("ray=Wall d=4.00 ny=1.00"));
        assert(host.Logged("ray2=miss"));
        assert(host.Logged("angvel=2.0"));

        // --- events: payload delivered, one throwing handler doesn't stop the rest ---
        assert(host.Logged("h1:42"));
        assert(host.Logged("h3:42"));   // ran despite h2 erroring
        bool sawBoom = false;
        for (size_t i = 0; i < host.errors.size(); ++i)
            if (host.errors[i].find("boom") != std::string::npos) sawBoom = true;
        assert(sawBoom);

        // --- timers: one-shot fires once, repeat repeats, cancel stops it ---
        host.logs.clear();
        host.errors.clear();
        for (int step = 0; step < 12; ++step)
            vm.Update(0.25f);   // 3.0s total

        assert(host.CountLogged("after@0.50") == 1); // one-shot, at its deadline
        assert(host.CountLogged("tick1") == 1);
        assert(host.CountLogged("tick2") == 1);
        assert(host.Logged("cancel=true"));
        assert(host.CountLogged("tick3") == 0);      // cancelled before the 3rd
        assert(!host.Logged("DELTA MISMATCH"));      // time.delta tracks dt
        assert(host.errors.empty());

        // --- trigger phases route to three different handlers ---
        host.logs.clear();
        vm.FireTrigger(0, 1, 0, phys::PhaseEnter);
        vm.FireTrigger(0, 1, 0, phys::PhaseStay);
        vm.FireTrigger(0, 2, 0, phys::PhaseExit);
        assert(host.Logged("enter:Wall"));
        assert(host.Logged("stay:Wall"));
        assert(host.Logged("exit:WallB"));

        // --- collisions reach both sides, each seeing the other ---
        host.logs.clear();
        vm.FireCollision(0, 1, phys::PhaseEnter);
        vm.FireCollision(0, 2, phys::PhaseExit);
        assert(host.Logged("hit:Wall:enter"));
        assert(host.Logged("hit:WallB:exit"));
        assert(host.errors.empty());
    }

    // A script that never calls the new APIs must be unaffected, and a missing
    // handler must stay free rather than erroring once per frame.
    void TestQuietScript()
    {
        StubHost host;
        host.names.push_back("Thing");

        phys::PhysicsWorld world;
        script::ScriptVM vm;
        vm.Begin(&world, &host, 0);
        vm.LoadScript(0, "function on_start() log('quiet') end\n", "quiet.lua");
        vm.Start();
        for (int i = 0; i < 5; ++i) vm.Update(0.016f);
        vm.FireTrigger(0, 0, 0, phys::PhaseStay);
        vm.FireCollision(0, 0, phys::PhaseEnter);

        assert(host.Logged("quiet"));
        assert(host.errors.empty());
    }

    // Clear() must drop the scheduler with the VM: a timer armed in one session
    // cannot fire in the next.
    void TestClearResetsTimers()
    {
        StubHost host;
        host.names.push_back("Thing");
        phys::PhysicsWorld world;

        script::ScriptVM vm;
        vm.Begin(&world, &host, 0);
        vm.LoadScript(0, "function on_start() time.after(1.0, function() log('stale') end) end\n",
                      "t.lua");
        vm.Start();

        vm.Begin(&world, &host, 0);          // restart, no scripts loaded
        for (int i = 0; i < 10; ++i) vm.Update(0.5f);
        assert(!host.Logged("stale"));
        assert(host.errors.empty());
    }

    // Phase 2: transforms/visibility/tags/camera on objects the simulation does
    // NOT own. Before this, obj:set_transform on a prop was a silent no-op.
    const char* kSceneScript =
        "function on_start()\n"
        "  local prop = find('Prop')\n"
        "  prop:set_position(1, 2, 3)\n"
        "  local x, y, z = prop:position()\n"
        "  log('prop=' .. x .. ',' .. y .. ',' .. z)\n"
        "  prop:set_scale(2)\n"
        "  local sx, sy, sz = prop:scale()\n"
        "  log('scale=' .. sx .. ',' .. sy .. ',' .. sz)\n"
        "  prop:set_rotation(0, 90, 0)\n"
        "  local _, ry, _ = prop:rotation()\n"
        "  log('yaw=' .. ry)\n"
        "  log('vis=' .. tostring(prop:visible()))\n"
        "  prop:hide()\n"
        "  log('hidden=' .. tostring(prop:visible()))\n"
        "  prop:show()\n"
        "  log('shown=' .. tostring(prop:visible()))\n"
        // A physics body must still route to the simulation, not the scene.
        "  local body = find('Body')\n"
        "  body:set_position(0, 7, 0)\n"
        "  local _, by, _ = body:position()\n"
        "  log('body_y=' .. string.format('%.2f', by))\n"
        "  log('add=' .. tostring(prop:add_tag('pickup')))\n"
        "  log('again=' .. tostring(prop:add_tag('pickup')))\n"
        "  log('has=' .. tostring(prop:has_tag('pickup')))\n"
        "  log('tagged=' .. #find_by_tag('pickup') .. ' authored=' .. #find_by_tag('scenery'))\n"
        "  log('ntags=' .. #prop:tags() .. ' first=' .. prop:tags()[1])\n"
        "  log('rm=' .. tostring(prop:remove_tag('pickup')))\n"
        "  log('has2=' .. tostring(prop:has_tag('pickup')))\n"
        "  log('cam0=' .. camera.active():name())\n"
        "  log('set=' .. tostring(camera.set_active(find('CamB'))))\n"
        "  log('cam1=' .. camera.active():name())\n"
        "  log('setprop=' .. tostring(camera.set_active(prop)))\n"
        "end\n";

    void TestLiveScene()
    {
        StubHost host;
        host.names.push_back("Body");   // 0: has a rigid body
        host.names.push_back("Prop");   // 1: no physics at all
        host.names.push_back("CamA");   // 2
        host.names.push_back("CamB");   // 3
        host.objects.resize(4);
        host.objects[1].tags.push_back("scenery");
        host.objects[2].isCamera = true;
        host.objects[2].camActive = true;
        host.objects[3].isCamera = true;

        std::vector<phys::BodyDesc> bodies;
        phys::BodyDesc body;
        body.kind = phys::BodyDesc::Kinematic;
        body.shape = phys::BodyDesc::Box;
        body.objectIndex = 0;
        bodies.push_back(body);
        phys::PhysicsWorld world;
        world.Build(bodies);

        script::ScriptVM vm;
        vm.Begin(&world, &host, 0);
        vm.LoadScript(1, kSceneScript, "scene.lua");
        vm.Start();

        if (!host.errors.empty()) host.Dump();
        assert(host.errors.empty());

        assert(host.Logged("prop=1.0,2.0,3.0"));
        assert(host.Logged("scale=2.0,2.0,2.0"));   // one arg scales uniformly
        assert(host.Logged("yaw=90.0"));
        assert(host.Logged("vis=true"));
        assert(host.Logged("hidden=false"));
        assert(host.Logged("shown=true"));
        assert(host.Logged("body_y=7.00"));         // routed through PhysicsWorld

        // The scene really changed, not just the script's view of it.
        assert(host.objects[1].pos[0] == 1.0f && host.objects[1].pos[2] == 3.0f);
        assert(host.objects[1].scale[1] == 2.0f);
        assert(host.objects[1].visible);
        // ...and the physics write did NOT leak into the scene transform.
        assert(host.objects[0].pos[1] == 0.0f);

        assert(host.Logged("add=true"));
        assert(host.Logged("again=false"));         // no duplicate tags
        assert(host.Logged("has=true"));
        assert(host.Logged("tagged=1 authored=1")); // authored tags are queryable too
        assert(host.Logged("ntags=2 first=scenery"));
        assert(host.Logged("rm=true"));
        assert(host.Logged("has2=false"));

        assert(host.Logged("cam0=CamA"));
        assert(host.Logged("set=true"));
        assert(host.Logged("cam1=CamB"));
        assert(host.Logged("setprop=false"));       // Prop has no Camera attribute
        assert(host.GetActiveCamera() == 3);
    }

    // Phase 3: obj:get / obj:set over the shared field table.
    const char* kAttrScript =
        "function on_start()\n"
        "  local lamp = find('Lamp')\n"
        "  log('n=' .. lamp:attr_count() .. ' t1=' .. lamp:attr_type(1)\n"
        "      .. ' t2=' .. lamp:attr_type(2))\n"
        "  log('intensity=' .. lamp:get('light_intensity'))\n"
        "  log('set=' .. tostring(lamp:set('light_intensity', 4.5)))\n"
        "  log('after=' .. lamp:get('light_intensity'))\n"
        "  local r, g, b = lamp:get('light_color')\n"
        "  log('color=' .. r .. ',' .. g .. ',' .. b)\n"
        "  lamp:set('light_color', 1, 0.5, 0)\n"
        "  local r2, g2, b2 = lamp:get('light_color')\n"
        "  log('color2=' .. r2 .. ',' .. g2 .. ',' .. b2)\n"
        "  lamp:set('light_color', 0.25)\n"          // one arg = all three
        "  local r3, g3, b3 = lamp:get('light_color')\n"
        "  log('color3=' .. r3 .. ',' .. g3 .. ',' .. b3)\n"
        "  log('mode=' .. lamp:get('light_mode'))\n"
        "  lamp:set('light_mode', 2)\n"
        "  log('mode2=' .. lamp:get('light_mode'))\n"
        "  log('volum=' .. tostring(lamp:get('light_volumetric')))\n"
        "  lamp:set('light_volumetric', true)\n"
        "  log('volum2=' .. tostring(lamp:get('light_volumetric')))\n"
        "  log('model=' .. lamp:get('model_path'))\n"
        "  lamp:set('model_path', 'assets/other.mesh')\n"
        "  log('model2=' .. lamp:get('model_path'))\n"
        // Explicit attribute index (1-based) targets one attribute exactly.
        "  log('fov=' .. lamp:get('cam_fov', 3))\n"
        "  lamp:set('cam_fov', 70, 3)\n"
        "  log('fov2=' .. lamp:get('cam_fov', 3))\n"
        // Unknown fields fail loudly-but-safely rather than corrupting memory.
        "  log('bogus=' .. tostring(lamp:get('not_a_field')))\n"
        "  log('bogusset=' .. tostring(lamp:set('not_a_field', 1)))\n"
        "end\n";

    void TestAttrFields()
    {
        StubHost host;
        host.names.push_back("Lamp");
        host.objects.resize(1);

        ObjectAttribute model;  model.type = "3D Model"; model.model_path = "assets/lamp.mesh";
        ObjectAttribute light;  light.type = "Point Light"; light.light_intensity = 2.0f;
        ObjectAttribute cam;    cam.type = "Camera";     cam.cam_fov = 55.0f;
        host.objects[0].attrs.push_back(model);
        host.objects[0].attrs.push_back(light);
        host.objects[0].attrs.push_back(cam);

        phys::PhysicsWorld world;
        script::ScriptVM vm;
        vm.Begin(&world, &host, 0);
        vm.LoadScript(0, kAttrScript, "attr.lua");
        vm.Start();

        if (!host.errors.empty()) host.Dump();
        assert(host.errors.empty());

        assert(host.Logged("n=3 t1=3D Model t2=Point Light"));
        // No index given: the non-default value wins, so the light is found even
        // though every attribute struct carries a light_intensity member.
        assert(host.Logged("intensity=2.0"));
        assert(host.Logged("set=true"));
        assert(host.Logged("after=4.5"));
        assert(host.objects[0].attrs[1].light_intensity == 4.5f);
        assert(host.objects[0].attrs[0].light_intensity == 1.0f); // untouched

        assert(host.Logged("color=1.0,1.0,1.0"));
        assert(host.Logged("color2=1.0,0.5,0.0"));
        assert(host.Logged("color3=0.25,0.25,0.25"));
        assert(host.Logged("mode=1"));
        assert(host.Logged("mode2=2"));
        assert(host.Logged("volum=false"));
        assert(host.Logged("volum2=true"));
        assert(host.Logged("model=assets/lamp.mesh"));
        assert(host.Logged("model2=assets/other.mesh"));
        assert(host.Logged("fov=55.0"));
        assert(host.Logged("fov2=70.0"));
        assert(host.objects[0].attrs[2].cam_fov == 70.0f);
        assert(host.Logged("bogus=nil"));
        assert(host.Logged("bogusset=false"));
    }

    // Every field in the shared list must round-trip through Get/Set, and the
    // table must be non-trivial — a truncated macro expansion would otherwise
    // pass every other test silently.
    void TestFieldTable()
    {
        int count = 0;
        const attrfields::FieldDesc* table = attrfields::Table(count);
        assert(table && count > 80);

        ObjectAttribute a;
        for (int i = 0; i < count; ++i)
        {
            const attrfields::FieldDesc& f = table[i];
            assert(attrfields::Find(f.name) == &f); // names are unique

            attrfields::Value wrote;
            wrote.kind = f.kind;
            switch (f.kind)
            {
            case attrfields::KindFloat: wrote.f[0] = 3.5f; break;
            case attrfields::KindInt:   wrote.i    = 7;    break;
            case attrfields::KindBool:  wrote.b[0] = true; break;
            case attrfields::KindStr:   wrote.s    = "xyz"; break;
            case attrfields::KindVec3:
                wrote.f[0] = 1.0f; wrote.f[1] = 2.0f; wrote.f[2] = 3.0f; break;
            case attrfields::KindBool3:
                wrote.b[0] = true; wrote.b[1] = false; wrote.b[2] = true; break;
            }
            assert(attrfields::Set(&a, f, wrote));

            attrfields::Value read;
            assert(attrfields::Get(&a, f, read));
            assert(attrfields::Equal(wrote, read));
        }
    }

    // Phase 4: spawn / destroy, and the property the whole slot scheme exists
    // for — a handle to a destroyed object must never resolve to whatever spawn
    // puts in its place.
    const char* kSpawnScript =
        "kept = nil\n"
        "function on_start()\n"
        "  local template = find('Template')\n"
        "  local a = spawn(template, 'Bullet1', 5, 0, 0)\n"
        "  log('spawned=' .. a:name() .. ' alive=' .. tostring(a:alive()))\n"
        "  local x, y, z = a:position()\n"
        "  log('at=' .. x .. ',' .. y .. ',' .. z)\n"
        "  log('tagcopy=' .. tostring(a:has_tag('ammo')))\n"
        "  log('count=' .. #find_all())\n"
        // Destroy it and keep the now-stale handle around.
        "  kept = a\n"
        "  log('destroy=' .. tostring(a:destroy()))\n"
        "  log('deadalive=' .. tostring(a:alive()))\n"
        "  log('count2=' .. #find_all())\n"
        "  log('deadname=' .. kept:name())\n"      // resolves to nothing, not a crash
        // Spawning again reuses the freed slot with a bumped generation.
        "  local b = spawn(template, 'Bullet2', 9, 0, 0)\n"
        "  log('reused=' .. tostring(b:id() == kept:id()))\n"
        "  log('newalive=' .. tostring(b:alive()) .. ' oldalive=' .. tostring(kept:alive()))\n"
        "  log('newname=' .. b:name() .. ' oldname=' .. kept:name())\n"
        // The stale handle must not be able to move the new object.
        "  kept:set_position(-100, -100, -100)\n"
        "  local bx = b:position()\n"
        "  log('untouched=' .. bx)\n"
        "  log('spawnbad=' .. tostring(spawn(kept, 'Nope', 0, 0, 0)))\n"
        "  log('destroytwice=' .. tostring(kept:destroy()))\n"
        "end\n";

    void TestSpawnDestroy()
    {
        StubHost host;
        host.names.push_back("Template");
        host.objects.resize(1);
        host.objects[0].tags.push_back("ammo");
        host.Populate();

        phys::PhysicsWorld world;
        script::ScriptVM vm;
        vm.Begin(&world, &host, 0);
        vm.LoadScript(0, kSpawnScript, "spawn.lua");
        vm.Start();

        if (!host.errors.empty()) host.Dump();
        assert(host.errors.empty());

        assert(host.Logged("spawned=Bullet1 alive=true"));
        assert(host.Logged("at=5.0,0.0,0.0"));
        assert(host.Logged("tagcopy=true"));      // the clone inherits the template
        assert(host.Logged("count=2"));
        assert(host.Logged("destroy=true"));
        assert(host.Logged("deadalive=false"));   // dead the instant destroy returns
        assert(host.Logged("count2=1"));          // and gone from enumeration
        assert(host.Logged("deadname="));         // a stale handle resolves to nothing

        assert(host.Logged("reused=true"));       // the freed slot really is reused
        assert(host.Logged("newalive=true oldalive=false"));
        assert(host.Logged("newname=Bullet2 oldname="));
        assert(host.Logged("untouched=9.0"));     // stale handle can't move it
        assert(host.objects[1].pos[0] == 9.0f);
        assert(host.Logged("spawnbad=nil"));
        assert(host.Logged("destroytwice=false"));
    }

    // The loading-screen contract: the script that started the load keeps running
    // (so its gui panel stays up and it can drive a progress bar), and the scene
    // only changes once assets are resident AND the minimum time has passed.
    const char* kLoadScript =
        "ticks = 0\n"
        "seen = {}\n"
        "function on_start()\n"
        "  log('begin=' .. tostring(scene.load('Level2', 1.0)))\n"
        "  log('again=' .. tostring(scene.load('Level3', 1.0)))\n"  // already loading
        "  log('loading=' .. tostring(scene.is_loading()))\n"
        "end\n"
        "function on_update(dt)\n"
        "  ticks = ticks + 1\n"                      // proves the script stays alive
        "  seen[#seen + 1] = scene.progress()\n"
        "  if not scene.is_loading() and not done then\n"
        "    done = true\n"
        "    log('arrived=' .. scene.name() .. ' afterticks=' .. ticks)\n"
        "    log('progress_now=' .. scene.progress())\n"
        "  end\n"
        "end\n";

    void TestSceneLoad()
    {
        StubHost host;
        host.names.push_back("Manager");
        host.objects.resize(1);
        host.sceneName = "Level1";

        phys::PhysicsWorld world;
        script::ScriptVM vm;
        vm.Begin(&world, &host, 0);
        vm.LoadScript(0, kLoadScript, "load.lua");
        vm.Start();

        assert(host.errors.empty());
        assert(host.Logged("begin=true"));
        assert(host.Logged("again=false"));     // a second load is refused
        assert(host.Logged("loading=true"));

        // Assets are not resident yet: no hand-over however long we wait.
        for (int i = 0; i < 30; ++i) { vm.Update(0.1f); host.TickLoad(0.1f); }
        assert(host.SceneIsLoading());
        assert(host.handovers == 0);
        assert(host.sceneName == "Level1");     // still the old scene

        // Assets land, and the minimum display time has long since elapsed, so
        // the hand-over happens on this tick.
        host.residency = 1.0f;
        vm.Update(0.1f); host.TickLoad(0.1f);   // frame 31: scripts run, THEN hand over
        assert(!host.SceneIsLoading());
        assert(host.handovers == 1);

        // The script only observes the new scene on the following frame, because
        // the load is ticked after every on_update — same ordering as the runtime.
        vm.Update(0.1f); host.TickLoad(0.1f);
        assert(host.Logged("arrived=Level2 afterticks=32"));
        assert(host.Logged("progress_now=1.0"));
        assert(host.errors.empty());
    }

    // The minimum display time is what stops a fast load flashing a loading
    // screen for two frames.
    void TestSceneLoadMinTime()
    {
        StubHost host;
        host.names.push_back("Manager");
        host.objects.resize(1);
        host.sceneName = "Level1";

        phys::PhysicsWorld world;
        script::ScriptVM vm;
        vm.Begin(&world, &host, 0);
        vm.LoadScript(0, "function on_start() scene.load('Fast', 0.5) end\n", "l.lua");
        vm.Start();

        host.residency = 1.0f;              // instantly resident
        for (int i = 0; i < 4; ++i) { vm.Update(0.1f); host.TickLoad(0.1f); }
        assert(host.SceneIsLoading());      // 0.4s < 0.5s minimum: still held
        assert(host.handovers == 0);

        vm.Update(0.1f); host.TickLoad(0.1f);
        assert(!host.SceneIsLoading());     // 0.5s reached
        assert(host.handovers == 1);
        assert(host.sceneName == "Fast");
    }

    // Progress must be the real read progress and nothing else — no clock term,
    // no padding. Time passing must not move the bar, and a load with nothing
    // left to read must hand over immediately.
    void TestSceneLoadProgressIsReal()
    {
        phys::PhysicsWorld world;

        // Still streaming: the bar tracks bytes, and sitting still for 5 seconds
        // does not advance it one bit.
        {
            StubHost host;
            host.names.push_back("Manager");
            host.objects.resize(1);
            host.sceneName = "Level1";

            script::ScriptVM vm;
            vm.Begin(&world, &host, 0);
            vm.LoadScript(0, "function on_start() scene.load('Big') end\n", "l.lua");
            vm.Start();

            host.residency = 0.20f;
            for (int i = 0; i < 20; ++i) { vm.Update(0.25f); host.TickLoad(0.25f); }
            assert(Near(host.SceneProgress(), 0.20f));   // 5s elapsed, still 20%
            assert(host.handovers == 0);

            host.residency = 0.65f;
            vm.Update(0.25f); host.TickLoad(0.25f);
            assert(Near(host.SceneProgress(), 0.65f));
            assert(host.handovers == 0);

            host.residency = 1.0f;
            vm.Update(0.25f); host.TickLoad(0.25f);
            assert(!host.SceneIsLoading());
            assert(host.handovers == 1);
            assert(host.sceneName == "Big");
        }

        // Everything already cached: no artificial wait, hand-over on the first
        // tick rather than padding the screen out.
        {
            StubHost host;
            host.names.push_back("Manager");
            host.objects.resize(1);
            host.sceneName = "Level1";

            script::ScriptVM vm;
            vm.Begin(&world, &host, 0);
            vm.LoadScript(0, "function on_start() scene.load('Cached') end\n", "l2.lua");
            vm.Start();

            host.residency = 1.0f;
            vm.Update(0.016f); host.TickLoad(0.016f);
            assert(!host.SceneIsLoading());
            assert(host.handovers == 1);
            assert(host.sceneName == "Cached");
        }
    }

    // A spawned object with a Rigid Body must join the live simulation, and a
    // destroyed one must leave it.
    void TestSpawnPhysics()
    {
        phys::PhysicsWorld world;
        std::vector<phys::BodyDesc> bodies;
        phys::BodyDesc floor;
        floor.kind = phys::BodyDesc::Static;
        floor.shape = phys::BodyDesc::Box;
        floor.halfExtents[0] = floor.halfExtents[1] = floor.halfExtents[2] = 1.0f;
        floor.pos[1] = -1.0f;
        floor.objectIndex = 0;
        bodies.push_back(floor);
        world.Build(bodies);

        assert(world.HasBody(0));
        assert(!world.HasBody(1));

        phys::BodyDesc spawned;
        spawned.kind = phys::BodyDesc::Dynamic;
        spawned.shape = phys::BodyDesc::Box;
        spawned.halfExtents[0] = spawned.halfExtents[1] = spawned.halfExtents[2] = 0.5f;
        spawned.mass = 1.0f;
        spawned.pos[1] = 10.0f;
        spawned.objectIndex = 1;
        assert(world.AddBody(spawned));
        assert(world.HasBody(1));

        // It falls, so it is genuinely in the simulation.
        for (int i = 0; i < 30; ++i) world.Step(1.0f / 60.0f);
        float p[3] = { 0.0f, 0.0f, 0.0f };
        assert(world.GetPosition(1, p));
        assert(p[1] < 9.9f);

        world.RemoveBody(1);
        assert(!world.HasBody(1));
        assert(world.HasBody(0));       // the floor is untouched
        assert(!world.GetPosition(1, p));

        // Repeated add/remove must not corrupt the world or leak it into a crash.
        for (int cycle = 0; cycle < 20; ++cycle)
        {
            assert(world.AddBody(spawned));
            world.Step(1.0f / 60.0f);
            world.RemoveBody(1);
        }
        assert(!world.HasBody(1));
        world.Step(1.0f / 60.0f);
    }
}

int main()
{
    TestScriptApi();
    TestQuietScript();
    TestClearResetsTimers();
    TestLiveScene();
    TestAttrFields();
    TestFieldTable();
    TestSpawnDestroy();
    TestSpawnPhysics();
    TestSceneLoad();
    TestSceneLoadMinTime();
    TestSceneLoadProgressIsReal();
    return 0;
}
