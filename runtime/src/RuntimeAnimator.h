#pragma once

#include "Content.h"
#include "RuntimeBoneModifiers.h"
#include "StreamPak.h"

#include <map>
#include <set>
#include <string>
#include <vector>

class StreamCache;

class RuntimeAnimator
{
public:
    RuntimeAnimator();

    struct Transform
    {
        float translation[3], rotation[4], scale[3];
    };

    struct BoneColliderPose
    {
        unsigned int boneHash;
        float halfExtents[3];
        float modelMatrix[16];
    };

    bool Load(StreamPak* pak, StreamCache* streamCache, const std::string& controllerPath,
              const std::string& initialState, float playbackSpeed, bool autoPlay);
    void Update(float deltaTime);
    bool BuildPalette(const RtMesh& mesh, const float objectWorld[16],
                      std::vector<float>& output);
    void GetBoneColliders(const RtMesh& mesh, std::vector<BoneColliderPose>& output) const;
    const char* BoneName(unsigned int hash) const;

    void SetFloat(const char* name, float value);
    void SetBool(const char* name, bool value);
    void SetTrigger(const char* name);
    void SetState(const char* name);
    bool IsValid() const { return m_entry != NULL && m_resource != NULL; }
    static void ClearSharedResources();

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
        unsigned int dataOffset, frameStride;
        float duration, sampleRate;
        bool loaded, frameMajor;
        std::vector<Track> tracks;
        unsigned int mappedSkeletonFingerprint;
        unsigned int mappedJointCount;
        std::vector<int> jointTracks;
        Clip() : idHash(0), offset(0), size(0), frameCount(0), dataOffset(0),
             frameStride(0), duration(0), sampleRate(0), loaded(false),
             frameMajor(false), mappedSkeletonFingerprint(0),
                 mappedJointCount(0) {}
    };
    struct Resource
    {
        unsigned int defaultState;
        std::vector<State> states;
        std::vector<Transition> transitions;
        std::vector<Clip> clips;
        std::vector<RuntimeBoneModifier> modifiers;
        Resource() : defaultState(0) {}
    };
    struct FrameCache
    {
        unsigned int clipHash, frame0, frame1;
        unsigned int prefetchClipHash, prefetchPageOffset;
        std::vector<unsigned char> samples0;
        std::vector<unsigned char> samples1;
        FrameCache() : clipHash(0), frame0(0), frame1(0),
                       prefetchClipHash(0), prefetchPageOffset(0) {}
    };
    const State* FindState(unsigned int hash) const;
    Clip* FindClip(unsigned int hash);
    bool FailResourceLoad();
    bool LoadClip(Clip& clip);
    bool Evaluate(const Transition& transition);
    bool SamplePalette(const RtMesh& mesh, const State& state, float time,
                       std::vector<float>& output, FrameCache& frameCache,
                       bool applyPhysics, const float objectWorld[16]);
    void PageForFrame(const Clip& clip, unsigned int frame, unsigned int& firstFrame,
                      unsigned int& offset, unsigned int& bytes) const;
    void DecodeTransform(const Track& track, const unsigned char* sample,
                         Transform& output) const;
    bool ReadTransform(const Clip& clip, const Track& track, unsigned int frame,
                       Transform& output);

    StreamPak* m_pak;
    StreamCache* m_streamCache;
    const SpakEntry* m_entry;
    Resource* m_resource;
    static std::map<const SpakEntry*, Resource*> s_resources;
    std::map<unsigned int, float> m_floatParameters;
    std::map<unsigned int, bool> m_boolParameters;
    std::set<unsigned int> m_triggers;
    unsigned int m_activeState, m_previousState;
    float m_stateTime, m_previousTime, m_previousSpeed;
    float m_blendDuration, m_blendRemaining;
    float m_lastDeltaTime;
    float m_playbackSpeed;
    bool m_autoPlay;
    std::vector<float> m_globalMatrices;
    std::vector<float> m_collisionMatrices;
    std::vector<float> m_previousPalette;
    std::vector<float> m_activePalette;
    FrameCache m_activeFrames;
    FrameCache m_previousFrames;
    unsigned int m_modifierSkeletonFingerprint;
    std::vector<int> m_jointModifiers;
    std::vector<RuntimeBonePhysicsState> m_physicsStates;
};