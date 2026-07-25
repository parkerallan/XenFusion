#pragma once

#include "Content.h"
#include "StreamPak.h"

#include <map>
#include <set>
#include <string>
#include <vector>

class RuntimeAnimator
{
public:
    RuntimeAnimator();

    struct Transform
    {
        float translation[3], rotation[4], scale[3];
    };

    bool Load(StreamPak* pak, const std::string& controllerPath,
              const std::string& initialState, float playbackSpeed, bool autoPlay);
    void Update(float deltaTime);
    bool BuildPalette(const RtMesh& mesh, std::vector<float>& output);

    void SetFloat(const char* name, float value);
    void SetBool(const char* name, bool value);
    void SetTrigger(const char* name);
    void SetState(const char* name);
    bool IsValid() const { return m_entry != NULL && !m_states.empty(); }

private:
    struct State
    {
        unsigned int nameHash, clipHash, flags;
        float speed;
    };
    struct Transition
    {
        unsigned int fromHash, toHash, parameterHash, operation, flags;
        float value, blendDuration, exitTime;
    };
    struct Track
    {
        unsigned int nameHash, sampleOffset;
        float translationMin[3], translationExtent[3];
        float scaleMin[3], scaleExtent[3];
    };
    struct Clip
    {
        unsigned int idHash, offset, size, frameCount;
        float duration, sampleRate;
        bool loaded;
        std::vector<Track> tracks;
        Clip() : idHash(0), offset(0), size(0), frameCount(0), duration(0),
                 sampleRate(0), loaded(false) {}
    };
    const State* FindState(unsigned int hash) const;
    Clip* FindClip(unsigned int hash);
    bool LoadClip(Clip& clip);
    bool Evaluate(const Transition& transition);
    bool SamplePalette(const RtMesh& mesh, const State& state, float time,
                       std::vector<float>& output);
    bool ReadTransform(const Clip& clip, const Track& track, unsigned int frame,
                       Transform& output);

    StreamPak* m_pak;
    const SpakEntry* m_entry;
    std::vector<State> m_states;
    std::vector<Transition> m_transitions;
    std::vector<Clip> m_clips;
    std::map<unsigned int, float> m_floatParameters;
    std::map<unsigned int, bool> m_boolParameters;
    std::set<unsigned int> m_triggers;
    unsigned int m_activeState, m_previousState;
    float m_stateTime, m_previousTime, m_previousSpeed;
    float m_blendDuration, m_blendRemaining;
    float m_playbackSpeed;
    bool m_autoPlay;
};