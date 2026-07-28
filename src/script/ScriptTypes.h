#pragma once

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

        // Route a script log() call to the host log ([LOG] in the editor).
        virtual void  Log(const char* msg) = 0;

        // Route a VM/script ERROR (parse error, runtime error in a handler) to the
        // host. Defaults to Log(); hosts override to surface it as an error.
        virtual void  LogError(const char* msg) { Log(msg); }

        // Resolve an object by name to its objectIndex, or -1 if not found.
        virtual int   FindObject(const char* name) = 0;

        // The name of the object at objectIndex ("" if out of range). The returned
        // pointer must stay valid for the duration of the call.
        virtual const char* ObjectName(int objectIndex) = 0;

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
    };
}
