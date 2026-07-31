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
namespace script { struct ScriptHost; }

namespace script
{
    class ScriptVM
    {
    public:
        ScriptVM();
        ~ScriptVM();

        // Start a fresh VM bound to a physics world + host. Clears any prior run.
        void Begin(phys::PhysicsWorld* phys, ScriptHost* host);

        // Compile a script for an object into its own environment. `name` is only
        // for error messages. Silently logs (via host) and skips on a parse error.
        void LoadScript(int objectIndex, const std::string& source, const std::string& name);

        void Start();                 // call each loaded on_start once
        void Update(float dt);        // call each on_update(dt)
        void FireTrigger(int objectIndex, int otherObjectIndex,
                 const char* boneName = 0); // on_trigger(other, bone)

        void Clear();                 // tear down the VM
        bool Empty() const;           // no scripts loaded

    private:
        struct Impl;
        Impl* m_impl;

        ScriptVM(const ScriptVM&);            // non-copyable
        ScriptVM& operator=(const ScriptVM&);
    };
}
