#pragma once

#include <string>

// A thin wrapper over Lua 5.4, shared by the editor preview and the Xbox 360
// runtime so scripts behave identically on both. The header hides Lua entirely
// (opaque impl), like PhysicsWorld hides Bullet. Strict C++03.
//
// Each scripted object runs its `on_start` / `on_update(dt)` / `on_trigger(other)`
// in an isolated environment holding a `self` handle. Scripts drive physics via
// self:apply_impulse / set_velocity / set_transform / position / velocity (routed
// to the shared PhysicsWorld), read input via input.button/axis, and log/find via
// the ScriptHost.

namespace phys   { class PhysicsWorld; }
namespace gui    { class Context; }
namespace script { struct ScriptHost; }

namespace script
{
    class ScriptVM
    {
    public:
        ScriptVM();
        ~ScriptVM();

        // Start a fresh VM bound to a physics world + host. Clears any prior run.
        // `guiContext` may be null on a host without a GUI backend — the gui.*
        // table is then absent and scripts that use it error loudly rather than
        // silently doing nothing.
        void Begin(phys::PhysicsWorld* phys, ScriptHost* host,
                   gui::Context* guiContext = 0);

        // Compile a script for an object into its own environment. `name` is only
        // for error messages. Silently logs (via host) and skips on a parse error.
        void LoadScript(int objectIndex, const std::string& source, const std::string& name);

        void Start();                 // call each loaded on_start once
        void Update(float dt);        // call each on_update(dt)

        // Trigger overlap dispatch. phase is phys::Phase (0=enter, 1=stay,
        // 2=exit) and selects the handler: on_trigger(other, bone) for enter
        // (unchanged signature), on_trigger_stay / on_trigger_exit otherwise.
        // Handlers that a script does not define cost nothing.
        void FireTrigger(int objectIndex, int otherObjectIndex,
                 const char* boneName = 0, int phase = 0);

        // Contact dispatch between two non-trigger bodies: each side's script
        // gets on_collision(other, phase) with `other` being the far object.
        void FireCollision(int aObjectIndex, int bObjectIndex, int phase);

        // An object is being destroyed: run its on_destroy, then drop its
        // environment so the slot is clean if a later spawn reuses it. Safe to
        // call for an object that has no script.
        void UnloadScript(int objectIndex);

        // Run on_start for a single object (a spawn, after its script loaded);
        // Start() runs it for everything loaded so far.
        void StartOne(int objectIndex);

        void Clear();                 // tear down the VM
        bool Empty() const;           // no scripts loaded

    private:
        void TickTimers(float dt); // time.after / time.every scheduler

        struct Impl;
        Impl* m_impl;

        ScriptVM(const ScriptVM&);            // non-copyable
        ScriptVM& operator=(const ScriptVM&);
    };
}
