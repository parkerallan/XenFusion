#pragma once

#include "scene/AttrFields.h"

// The tiny per-target surface the shared ScriptVM needs beyond physics: input,
// logging, and object lookup. The editor and the 360 runtime each implement this
// over their own input/log/scene structs; everything else the scripts touch
// (impulse, velocity, transform, position) routes through the shared PhysicsWorld
// by objectIndex. Strict C++03 (compiled by the XDK toolset).

namespace script
{
    struct ScriptHost
    {
        virtual ~ScriptHost() {}

        // Controller/keyboard state for a virtual button/axis name
        // ("A","B","X","Y","Start","DPadUp"... / "LX","LY","RX","RY","LT","RT").
        virtual bool  InputButton(const char* name) = 0;
        virtual float InputAxis(const char* name) = 0;

        // Edges: true only on the frame the button went down / came up. Held
        // state (InputButton) is true for every frame of a press, so anything
        // that should happen once per press must ask these. Defaulted to false so
        // a host that has no edge tracking still builds.
        virtual bool  InputButtonPressed(const char* name)
        { (void)name; return false; }
        virtual bool  InputButtonReleased(const char* name)
        { (void)name; return false; }

        // Route a script log() call to the host log ([LOG] in the editor).
        virtual void  Log(const char* msg) = 0;

        // Route a VM/script ERROR (parse error, runtime error in a handler) to the
        // host. Defaults to Log(); hosts override to surface it as an error.
        virtual void  LogError(const char* msg) { Log(msg); }

        // Resolve an object by name to its objectIndex, or -1 if not found.
        virtual int   FindObject(const char* name) = 0;

        // How many object slots exist, so scripts can enumerate the scene
        // (find_all / find_by_prefix walk [0, ObjectCount)).
        virtual int   ObjectCount() = 0;

        // --- Object lifetime --------------------------------------------------
        // Destroying an object frees its slot for reuse, so an index alone can no
        // longer identify it. Every Lua handle therefore carries the generation
        // the slot had when the handle was made, and a stale handle resolves to
        // nothing instead of aliasing whatever moved in — the same guarantee the
        // GUI widget handles already give.
        virtual unsigned ObjectGeneration(int objectIndex)
        { (void)objectIndex; return 0; }
        virtual bool  ObjectAlive(int objectIndex)
        { return objectIndex >= 0 && objectIndex < ObjectCount(); }

        // Clone `sourceObjectIndex` (its attributes, scale and rotation) into a
        // free or new slot at `pos`, named `name`. Returns the new objectIndex,
        // or -1. Cloning rather than loading a path is deliberate: the template's
        // assets are already cooked into the pak because the template is in the
        // scene, so a spawn needs no new residency path.
        virtual int   SpawnObject(int sourceObjectIndex, const char* name,
                                  const float pos[3])
        { (void)sourceObjectIndex; (void)name; (void)pos; return -1; }
        virtual bool  DestroyObject(int objectIndex)
        { (void)objectIndex; return false; }

        // --- Scene switching --------------------------------------------------
        // Begin a scene change. `name` is a scene name or scene-relative path.
        //
        // The swap is NOT immediate: the current scene keeps running and
        // rendering while the target's meshes stream in, so whatever the script
        // put on screen (a gui.* loading panel) stays up, and the script itself
        // stays alive to drive it. The hand-over happens once every mesh is
        // resident and at least `minSeconds` have passed — the minimum stops a
        // fast load from flashing a loading screen for two frames.
        virtual bool  LoadScene(const char* name, float minSeconds)
        { (void)name; (void)minSeconds; return false; }
        virtual bool  SceneIsLoading() { return false; }
        // 0..1 progress toward the hand-over: the lesser of mesh residency and
        // the minimum display time. The editor has nothing to stream, so only
        // the time term moves there.
        virtual float SceneProgress() { return 1.0f; }
        virtual const char* SceneName() { return ""; }

        // The name of the object at objectIndex ("" if out of range). The returned
        // pointer must stay valid for the duration of the call.
        virtual const char* ObjectName(int objectIndex) = 0;

        // --- Live scene state -------------------------------------------------
        // The object's transform in authored space (position, Euler degrees,
        // scale). Objects with a Rigid Body are driven by the PhysicsWorld
        // instead: the VM checks PhysicsWorld::HasBody and only falls back to
        // these for props the simulation doesn't own. Writes are transient and
        // never touch the authored scene file.
        //   mask bits: 1 = position, 2 = rotation, 4 = scale.
        enum { XformPos = 1, XformRot = 2, XformScale = 4 };
        virtual bool GetObjectTransform(int objectIndex, float pos[3], float rot[3],
                                        float scale[3])
        { (void)objectIndex; (void)pos; (void)rot; (void)scale; return false; }
        virtual void SetObjectTransform(int objectIndex, const float pos[3],
                                        const float rot[3], const float scale[3], int mask)
        { (void)objectIndex; (void)pos; (void)rot; (void)scale; (void)mask; }

        // Per-object visibility (the authored "visible" flag, overridden live).
        virtual bool GetObjectVisible(int objectIndex) { (void)objectIndex; return true; }
        virtual void SetObjectVisible(int objectIndex, bool visible)
        { (void)objectIndex; (void)visible; }

        // Active "Camera" attribute. Setting one clears whatever was active, so
        // at most one camera drives the view. Returns false if the object has no
        // Camera attribute.
        virtual bool SetActiveCamera(int objectIndex) { (void)objectIndex; return false; }
        virtual int  GetActiveCamera() { return -1; }

        // Object tags. Indexed rather than vector-valued so the interface stays
        // dependency-free (same shape as ObjectName). Script-side adds/removes
        // are live only — they never rewrite the scene.
        virtual int  ObjectTagCount(int objectIndex) { (void)objectIndex; return 0; }
        virtual const char* ObjectTag(int objectIndex, int tagIndex)
        { (void)objectIndex; (void)tagIndex; return ""; }
        virtual bool AddObjectTag(int objectIndex, const char* tag)
        { (void)objectIndex; (void)tag; return false; }
        virtual bool RemoveObjectTag(int objectIndex, const char* tag)
        { (void)objectIndex; (void)tag; return false; }

        // --- Attribute fields (obj:get / obj:set) -----------------------------
        // Read or write one named field on one of the object's attributes, using
        // the shared scene/AttrFields.h table. attrIndex < 0 means "the first
        // attribute on this object that has this field". Both return false when
        // the object, the attribute or the field doesn't exist.
        virtual bool AttrGet(int objectIndex, int attrIndex, const char* field,
                             attrfields::Value& out)
        { (void)objectIndex; (void)attrIndex; (void)field; (void)out; return false; }
        virtual bool AttrSet(int objectIndex, int attrIndex, const char* field,
                             const attrfields::Value& in)
        { (void)objectIndex; (void)attrIndex; (void)field; (void)in; return false; }
        // How many attributes the object has, and the type string of one of them
        // ("3D Model", "Point Light", ...) — so a script can find the right index.
        virtual int  AttrCount(int objectIndex) { (void)objectIndex; return 0; }
        virtual const char* AttrType(int objectIndex, int attrIndex)
        { (void)objectIndex; (void)attrIndex; return ""; }

        // Text attribute content control (the "text" Lua table). The override
        // is transient and does not modify the authored scene value.
        virtual void TextSetValue(int objectIndex, const char* value)
        { (void)objectIndex; (void)value; }

        // Video attribute control (the "video" Lua table). play() starts the
        // object's video from the top — once by default, looping when loop is
        // true; on a video that is already playing it only updates the mode.
        // stop() forces Off (releasing the decoder). Defaults are no-ops so
        // hosts without a video overlay still build.
        virtual void VideoSetPlaying(int objectIndex, bool play, bool loop)
        { (void)objectIndex; (void)play; (void)loop; }
        virtual bool VideoIsPlaying(int objectIndex) { (void)objectIndex; return false; }

        // Audio attribute control (the "audio" Lua table). play() starts the
        // object's clip from the top (restarting a stopped/finished one);
        // stop() silences it; volume/pitch/loop apply live. Defaults are
        // no-ops so hosts without audio still build.
        virtual void AudioSetPlaying(int objectIndex, bool play)
        { (void)objectIndex; (void)play; }
        virtual bool AudioIsPlaying(int objectIndex) { (void)objectIndex; return false; }
        virtual void AudioSetVolume(int objectIndex, float volume)
        { (void)objectIndex; (void)volume; }
        virtual void AudioSetPitch(int objectIndex, float pitch)
        { (void)objectIndex; (void)pitch; }
        virtual void AudioSetLoop(int objectIndex, bool loop)
        { (void)objectIndex; (void)loop; }

        virtual void AnimatorSetFloat(int objectIndex, const char* name, float value)
        { (void)objectIndex; (void)name; (void)value; }
        virtual void AnimatorSetBool(int objectIndex, const char* name, bool value)
        { (void)objectIndex; (void)name; (void)value; }
        virtual void AnimatorSetTrigger(int objectIndex, const char* name)
        { (void)objectIndex; (void)name; }
        virtual void AnimatorSetState(int objectIndex, const char* name)
        { (void)objectIndex; (void)name; }

        // Facial animation. The expression pose, a playing lip-sync clip and
        // gaze are separate layers: setting one leaves the others alone.
        virtual bool FaceSetPose(int objectIndex, const char* pose, float weight, float speed)
        { (void)objectIndex; (void)pose; (void)weight; (void)speed; return false; }
        virtual bool FaceLookAt(int objectIndex, float x, float y, float z)
        { (void)objectIndex; (void)x; (void)y; (void)z; return false; }
        virtual void FaceClearGaze(int objectIndex)
        { (void)objectIndex; }
        virtual void FaceSetBlink(int objectIndex, bool enabled)
        { (void)objectIndex; (void)enabled; }
    };
}
