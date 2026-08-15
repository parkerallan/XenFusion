#include "RuntimeAnimator.h"

#include "AnimationFormat.h"
#include "AnimatorCookFormat.h"
#include "Endian.h"
#include "SpakFormat.h"
#include "StreamCache.h"

#include <math.h>
#include <string.h>

namespace
{
    float ReadFloatBE(const unsigned char* data)
    {
        const unsigned int bits = endian::LoadU32BE(data);
        float value;
        memcpy(&value, &bits, sizeof(value));
        return value;
    }

    float Clamp(float value, float minimum, float maximum)
    {
        return value < minimum ? minimum : (value > maximum ? maximum : value);
    }

    struct Matrix
    {
        float m[4][4];
    };

    Matrix MultiplyMatrix(const Matrix& left, const Matrix& right)
    {
        Matrix result;
        for (int row = 0; row < 4; ++row)
            for (int column = 0; column < 4; ++column)
                result.m[row][column] =
                    left.m[row][0] * right.m[0][column] +
                    left.m[row][1] * right.m[1][column] +
                    left.m[row][2] * right.m[2][column] +
                    left.m[row][3] * right.m[3][column];
        return result;
    }

    Matrix Compose(const RuntimeAnimator::Transform& transform)
    {
        const float x = transform.rotation[0], y = transform.rotation[1];
        const float z = transform.rotation[2], w = transform.rotation[3];
        Matrix result;
        result.m[0][0] = (1.0f - 2.0f * (y * y + z * z)) * transform.scale[0];
        result.m[0][1] = (2.0f * (x * y - z * w)) * transform.scale[0];
        result.m[0][2] = (2.0f * (x * z + y * w)) * transform.scale[0];
        result.m[0][3] = transform.translation[0];
        result.m[1][0] = (2.0f * (x * y + z * w)) * transform.scale[1];
        result.m[1][1] = (1.0f - 2.0f * (x * x + z * z)) * transform.scale[1];
        result.m[1][2] = (2.0f * (y * z - x * w)) * transform.scale[1];
        result.m[1][3] = transform.translation[1];
        result.m[2][0] = (2.0f * (x * z - y * w)) * transform.scale[2];
        result.m[2][1] = (2.0f * (y * z + x * w)) * transform.scale[2];
        result.m[2][2] = (1.0f - 2.0f * (x * x + y * y)) * transform.scale[2];
        result.m[2][3] = transform.translation[2];
        result.m[3][0] = result.m[3][1] = result.m[3][2] = 0.0f;
        result.m[3][3] = 1.0f;
        return result;
    }

    void InterpolateQuaternion(const float* first, const float* second,
                               float alpha, float* output)
    {
        float adjusted[4] = { second[0], second[1], second[2], second[3] };
        float cosine = first[0] * second[0] + first[1] * second[1] +
                       first[2] * second[2] + first[3] * second[3];
        if (cosine < 0.0f)
        {
            cosine = -cosine;
            for (int component = 0; component < 4; ++component) adjusted[component] = -adjusted[component];
        }
        float firstWeight = 1.0f - alpha, secondWeight = alpha;
        if (cosine < 0.9995f)
        {
            const float angle = acosf(Clamp(cosine, -1.0f, 1.0f));
            const float divisor = sinf(angle);
            firstWeight = sinf((1.0f - alpha) * angle) / divisor;
            secondWeight = sinf(alpha * angle) / divisor;
        }
        float lengthSquared = 0.0f;
        for (int component = 0; component < 4; ++component)
        {
            output[component] = first[component] * firstWeight + adjusted[component] * secondWeight;
            lengthSquared += output[component] * output[component];
        }
        const float inverseLength = lengthSquared > 1e-12f ? 1.0f / sqrtf(lengthSquared) : 1.0f;
        for (int component = 0; component < 4; ++component) output[component] *= inverseLength;
    }
}

std::map<const SpakEntry*, RuntimeAnimator::Resource*> RuntimeAnimator::s_resources;

RuntimeAnimator::RuntimeAnimator()
        : m_pak(NULL), m_streamCache(NULL), m_entry(NULL), m_resource(NULL),
            m_activeState(0), m_previousState(0),
      m_stateTime(0), m_previousTime(0), m_previousSpeed(1),
      m_blendDuration(0), m_blendRemaining(0), m_playbackSpeed(1), m_autoPlay(true)
    , m_lastDeltaTime(0), m_modifierSkeletonFingerprint(0)
{
}

bool RuntimeAnimator::Load(StreamPak* pak, StreamCache* streamCache,
                           const std::string& controllerPath,
                           const std::string& initialState, float playbackSpeed, bool autoPlay)
{
    m_entry = NULL;
    m_resource = NULL;
    m_pak = pak;
    m_streamCache = streamCache;
    m_entry = pak && pak->IsOpen() ? pak->Find(spak::NameHash(controllerPath.c_str())) : NULL;
    if (!m_entry || m_entry->type != spak::kTypeAnim || (m_entry->flags & spak::kFlagCompressed))
        return false;
    std::map<const SpakEntry*, Resource*>::iterator shared = s_resources.find(m_entry);
    if (shared != s_resources.end())
    {
        m_resource = shared->second;
        m_activeState = spak::NameHash(initialState.c_str());
        if (!FindState(m_activeState)) m_activeState = m_resource->defaultState;
        if (!FindState(m_activeState)) m_activeState = m_resource->states[0].nameHash;
        m_playbackSpeed = playbackSpeed > 0.0f ? playbackSpeed : 0.0f;
        m_autoPlay = autoPlay;
        return true;
    }
    m_resource = new Resource();
    unsigned char header[animcook::kHeaderBytes];
    if (m_entry->uncompressedSize < animcook::kHeaderBytesV1 ||
        !m_pak->ReadRawRange(m_entry, 0, header,
            m_entry->uncompressedSize < sizeof(header) ? m_entry->uncompressedSize : sizeof(header)) ||
        endian::LoadU32BE(header) != animcook::kMagic)
        return FailResourceLoad();
    const unsigned int version = endian::LoadU32BE(header + 4);
    if (version != animcook::kVersionV1 && version != animcook::kVersionV2 &&
        version != animcook::kVersion)
        return FailResourceLoad();
    const unsigned int stateCount = endian::LoadU32BE(header + 12);
    const unsigned int transitionCount = endian::LoadU32BE(header + 16);
    const unsigned int clipCount = endian::LoadU32BE(header + 20);
    const unsigned int stateOffset = endian::LoadU32BE(header + 24);
    const unsigned int transitionOffset = endian::LoadU32BE(header + 28);
    const unsigned int clipOffset = endian::LoadU32BE(header + 32);
    const unsigned int modifierCount = version >= animcook::kVersionV2
        ? endian::LoadU32BE(header + 40) : 0;
    const unsigned int modifierOffset = version >= animcook::kVersionV2
        ? endian::LoadU32BE(header + 44) : 0;
    const unsigned int stringOffset = version >= animcook::kVersionV2
        ? endian::LoadU32BE(header + 48) : 0;
    const unsigned int stringBytes = version >= animcook::kVersionV2
        ? endian::LoadU32BE(header + 52) : 0;
    if (stateCount == 0 || stateCount > 1024 || transitionCount > 4096 || clipCount == 0 || clipCount > 1024 ||
        modifierCount > 1024 ||
        stateOffset + stateCount * animcook::kStateBytes > m_entry->uncompressedSize ||
        transitionOffset + transitionCount * animcook::kTransitionBytes > m_entry->uncompressedSize ||
        clipOffset + clipCount * animcook::kClipBytes > m_entry->uncompressedSize ||
        modifierOffset + modifierCount * animcook::kModifierBytes > m_entry->uncompressedSize ||
        stringOffset + stringBytes > m_entry->uncompressedSize)
        return FailResourceLoad();

    std::vector<unsigned char> records;
    records.resize(stateCount * animcook::kStateBytes);
    if (!m_pak->ReadRawRange(m_entry, stateOffset, &records[0], (unsigned int)records.size())) return FailResourceLoad();
    for (unsigned int index = 0; index < stateCount; ++index)
    {
        const unsigned char* record = &records[index * animcook::kStateBytes];
        State state;
        state.nameHash = endian::LoadU32BE(record);
        state.clipHash = endian::LoadU32BE(record + 4);
        state.speed = ReadFloatBE(record + 8);
        state.flags = endian::LoadU32BE(record + 12);
        m_resource->states.push_back(state);
    }
    records.resize(transitionCount * animcook::kTransitionBytes);
    if (transitionCount && !m_pak->ReadRawRange(m_entry, transitionOffset, &records[0], (unsigned int)records.size())) return FailResourceLoad();
    for (unsigned int index = 0; index < transitionCount; ++index)
    {
        const unsigned char* record = &records[index * animcook::kTransitionBytes];
        Transition transition;
        transition.fromHash = endian::LoadU32BE(record);
        transition.toHash = endian::LoadU32BE(record + 4);
        transition.parameterHash = endian::LoadU32BE(record + 8);
        transition.operation = endian::LoadU32BE(record + 12);
        transition.value = ReadFloatBE(record + 16);
        transition.blendDuration = ReadFloatBE(record + 20);
        transition.exitTime = ReadFloatBE(record + 24);
        transition.flags = endian::LoadU32BE(record + 28);
        m_resource->transitions.push_back(transition);
    }
    records.resize(clipCount * animcook::kClipBytes);
    if (!m_pak->ReadRawRange(m_entry, clipOffset, &records[0], (unsigned int)records.size())) return FailResourceLoad();
    for (unsigned int index = 0; index < clipCount; ++index)
    {
        const unsigned char* record = &records[index * animcook::kClipBytes];
        Clip clip;
        clip.idHash = endian::LoadU32BE(record);
        clip.offset = endian::LoadU32BE(record + 8);
        clip.size = endian::LoadU32BE(record + 12);
        if (clip.offset + clip.size > m_entry->uncompressedSize || clip.size < animfmt::kHeaderBytes) return FailResourceLoad();
        m_resource->clips.push_back(clip);
    }
    records.resize(modifierCount * animcook::kModifierBytes);
    if (modifierCount && !m_pak->ReadRawRange(m_entry, modifierOffset, &records[0],
                                               (unsigned int)records.size()))
        return FailResourceLoad();
    std::vector<unsigned char> strings(stringBytes);
    if (stringBytes && !m_pak->ReadRawRange(m_entry, stringOffset, &strings[0], stringBytes))
        return FailResourceLoad();
    for (unsigned int index = 0; index < modifierCount; ++index)
    {
        const unsigned char* record = &records[index * animcook::kModifierBytes];
        RuntimeBoneModifier modifier;
        modifier.boneHash = endian::LoadU32BE(record);
        modifier.type = endian::LoadU32BE(record + 4);
        modifier.flags = endian::LoadU32BE(record + 8);
        if (modifier.type > animcook::ModifierCollision)
            return FailResourceLoad();
        modifier.strength = ReadFloatBE(record + 16);
        modifier.damping = ReadFloatBE(record + 20);
        modifier.stiffness = ReadFloatBE(record + 24);
        modifier.mass = ReadFloatBE(record + 28);
        modifier.drag = ReadFloatBE(record + 32);
        modifier.gravityScale = ReadFloatBE(record + 36);
        for (int axis = 0; axis < 3; ++axis) modifier.gravityDir[axis] = ReadFloatBE(record + 40 + axis * 4);
        modifier.angleLimitDeg = ReadFloatBE(record + 52);
        modifier.radius = ReadFloatBE(record + 56);
        for (int axis = 0; axis < 3; ++axis) modifier.boxHalfExtents[axis] = ReadFloatBE(record + 60 + axis * 4);
        for (int axis = 0; axis < 3; ++axis) modifier.boxCenter[axis] = ReadFloatBE(record + 72 + axis * 4);
        const unsigned int nameOffset = endian::LoadU32BE(record + 84);
        const unsigned int nameLength = endian::LoadU32BE(record + 88);
        if (nameOffset > stringBytes || nameLength > stringBytes - nameOffset)
            return FailResourceLoad();
        if (nameLength) modifier.boneName.assign((const char*)&strings[nameOffset], nameLength);
        if (!(modifier.mass > 0.0f) || modifier.boxHalfExtents[0] < 0.0f ||
            modifier.boxHalfExtents[1] < 0.0f || modifier.boxHalfExtents[2] < 0.0f)
            return FailResourceLoad();
        m_resource->modifiers.push_back(modifier);
    }
    // Controllers cooked before facial animation existed stop at v2.
    if (version >= animcook::kVersion)
    {
        m_resource->defaultPose = endian::LoadU32BE(header + 56);
        const unsigned int poseCount        = endian::LoadU32BE(header + 60);
        const unsigned int poseOffset       = endian::LoadU32BE(header + 64);
        const unsigned int poseTargetOffset = endian::LoadU32BE(header + 68);
        if (poseCount > animcook::kMaxPoses ||
            poseOffset + poseCount * animcook::kPoseBytes > m_entry->uncompressedSize)
            return FailResourceLoad();

        if (poseCount)
        {
            records.resize(poseCount * animcook::kPoseBytes);
            if (!m_pak->ReadRawRange(m_entry, poseOffset, &records[0], (unsigned int)records.size()))
                return FailResourceLoad();
            // One flat array shared by every pose: read the span it ends at.
            unsigned int targetTotal = 0;
            for (unsigned int index = 0; index < poseCount; ++index)
            {
                const unsigned char* record = &records[index * animcook::kPoseBytes];
                targetTotal = endian::LoadU32BE(record + 4) + endian::LoadU32BE(record + 8);
            }
            std::vector<unsigned char> targets(targetTotal * animcook::kPoseTargetBytes);
            if (targetTotal &&
                (poseTargetOffset + targets.size() > m_entry->uncompressedSize ||
                 !m_pak->ReadRawRange(m_entry, poseTargetOffset, &targets[0],
                                      (unsigned int)targets.size())))
                return FailResourceLoad();

            for (unsigned int index = 0; index < poseCount; ++index)
            {
                const unsigned char* record = &records[index * animcook::kPoseBytes];
                face::Pose pose;
                pose.nameHash = endian::LoadU32BE(record);
                const unsigned int first = endian::LoadU32BE(record + 4);
                const unsigned int count = endian::LoadU32BE(record + 8);
                if (first + count > targetTotal)
                    return FailResourceLoad();
                for (unsigned int t = 0; t < count; ++t)
                {
                    const unsigned char* entry = &targets[(first + t) * animcook::kPoseTargetBytes];
                    face::PoseTarget target;
                    target.shape = entry[0];
                    target.weight = entry[1];
                    if (target.shape < face::kShapeCount)
                        pose.targets.push_back(target);
                }
                m_resource->poses.push_back(pose);
            }
        }

    }

    m_resource->defaultState = endian::LoadU32BE(header + 8);
    m_activeState = spak::NameHash(initialState.c_str());
    if (!FindState(m_activeState)) m_activeState = m_resource->defaultState;
    if (!FindState(m_activeState)) m_activeState = m_resource->states[0].nameHash;
    m_playbackSpeed = playbackSpeed > 0.0f ? playbackSpeed : 0.0f;
    m_autoPlay = autoPlay;
    for (size_t clipIndex = 0; clipIndex < m_resource->clips.size(); ++clipIndex)
        if (!LoadClip(m_resource->clips[clipIndex])) return FailResourceLoad();
    s_resources[m_entry] = m_resource;
    const State* initial = FindState(m_activeState);
    Clip* initialClip = initial ? FindClip(initial->clipHash) : NULL;
    if (initialClip && LoadClip(*initialClip) && initialClip->frameMajor && m_streamCache)
    {
        unsigned int firstFrame = 0, offset = 0, bytes = 0;
        PageForFrame(*initialClip, 0, firstFrame, offset, bytes);
        m_streamCache->GetRawRange(m_entry, offset, bytes, NULL);
    }
    for (size_t clipIndex = 0; clipIndex < m_resource->clips.size(); ++clipIndex)
    {
        Clip& clip = m_resource->clips[clipIndex];
        if (&clip == initialClip || !clip.frameMajor || !m_streamCache) continue;
        unsigned int firstFrame = 0, offset = 0, bytes = 0;
        PageForFrame(clip, 0, firstFrame, offset, bytes);
        m_streamCache->GetRawRange(m_entry, offset, bytes, NULL);
    }
    return true;
}

bool RuntimeAnimator::FailResourceLoad()
{
    delete m_resource;
    m_resource = NULL;
    m_entry = NULL;
    return false;
}

const RuntimeAnimator::State* RuntimeAnimator::FindState(unsigned int hash) const
{
    if (!m_resource) return NULL;
    for (size_t index = 0; index < m_resource->states.size(); ++index)
        if (m_resource->states[index].nameHash == hash) return &m_resource->states[index];
    return NULL;
}

const face::Pose* RuntimeAnimator::FindPose(unsigned int nameHash) const
{
    if (!m_resource || nameHash == 0) return NULL;
    for (size_t index = 0; index < m_resource->poses.size(); ++index)
        if (m_resource->poses[index].nameHash == nameHash) return &m_resource->poses[index];
    return NULL;
}


bool RuntimeAnimator::HasFace() const
{
    return m_resource && !m_resource->poses.empty();
}

RuntimeAnimator::Clip* RuntimeAnimator::FindClip(unsigned int hash)
{
    if (!m_resource) return NULL;
    for (size_t index = 0; index < m_resource->clips.size(); ++index)
        if (m_resource->clips[index].idHash == hash) return &m_resource->clips[index];
    return NULL;
}

bool RuntimeAnimator::Evaluate(const Transition& transition)
{
    if (transition.operation == animcook::CondAlways) return true;
    std::map<unsigned int, bool>::const_iterator boolean = m_boolParameters.find(transition.parameterHash);
    std::map<unsigned int, float>::const_iterator number = m_floatParameters.find(transition.parameterHash);
    if (transition.operation == animcook::CondTruthy || transition.operation == animcook::CondFalsy)
    {
        bool value = boolean != m_boolParameters.end() && boolean->second;
        std::set<unsigned int>::iterator trigger = m_triggers.find(transition.parameterHash);
        if (trigger != m_triggers.end()) { value = true; if (transition.operation == animcook::CondTruthy) m_triggers.erase(trigger); }
        return transition.operation == animcook::CondTruthy ? value : !value;
    }
    float left = 0.0f;
    if (transition.flags & animcook::kConditionBoolLiteral)
    {
        if (boolean == m_boolParameters.end()) return false;
        left = boolean->second ? 1.0f : 0.0f;
    }
    else
    {
        if (number == m_floatParameters.end()) return false;
        left = number->second;
    }
    if (transition.operation == animcook::CondEqual) return fabsf(left - transition.value) <= 0.0001f;
    if (transition.operation == animcook::CondNotEqual) return fabsf(left - transition.value) > 0.0001f;
    if (transition.operation == animcook::CondGreaterEqual) return left >= transition.value;
    if (transition.operation == animcook::CondLessEqual) return left <= transition.value;
    if (transition.operation == animcook::CondGreater) return left > transition.value;
    return left < transition.value;
}

void RuntimeAnimator::Update(float deltaTime)
{
    const State* active = FindState(m_activeState);
    if (!active) return;
    if (m_autoPlay)
    {
        const float delta = deltaTime > 0.0f ? deltaTime : 0.0f;
        m_stateTime += delta * m_playbackSpeed * (active->speed > 0.0f ? active->speed : 0.0f);
        if (m_previousState) m_previousTime += delta * m_playbackSpeed * m_previousSpeed;
    }
    m_lastDeltaTime = Clamp(deltaTime, 0.0f, 4.0f / 60.0f);
    if (m_blendRemaining > 0.0f)
    {
        m_blendRemaining -= deltaTime;
        if (m_blendRemaining <= 0.0f)
        { m_blendRemaining = m_blendDuration = 0.0f; m_previousState = 0; }
    }
    for (size_t index = 0; index < m_resource->transitions.size(); ++index)
    {
        const Transition& transition = m_resource->transitions[index];
        if (transition.fromHash != m_activeState ||
            ((transition.flags & animcook::kTransitionHasExitTime) && m_stateTime < transition.exitTime) ||
            !Evaluate(transition) || !FindState(transition.toHash)) continue;
        m_previousState = m_activeState;
        m_previousTime = m_stateTime;
        m_previousSpeed = active->speed > 0.0f ? active->speed : 0.0f;
        m_blendDuration = transition.blendDuration > 0.0f ? transition.blendDuration : 0.0f;
        m_blendRemaining = m_blendDuration;
        if (m_blendDuration == 0.0f) m_previousState = 0;
        m_activeState = transition.toHash;
        m_stateTime = 0.0f;
        break;
    }
}

bool RuntimeAnimator::LoadClip(Clip& clip)
{
    if (clip.loaded) return !clip.tracks.empty();
    clip.loaded = true;
    unsigned char header[animfmt::kHeaderBytes];
    if (!m_pak->ReadRawRange(m_entry, clip.offset, header, sizeof(header)))
        return false;
    const unsigned int magic = endian::LoadU32BE(header);
    const unsigned int version = endian::LoadU32BE(header + 4);
    clip.frameMajor = magic == animfmt::kMagicV2 && version == animfmt::kVersionV2;
    if (!clip.frameMajor && (magic != animfmt::kMagicV1 || version != animfmt::kVersionV1))
        return false;
    const unsigned int trackCount = endian::LoadU32BE(header + 8);
    clip.frameCount = endian::LoadU32BE(header + 12);
    clip.duration = ReadFloatBE(header + 16);
    clip.sampleRate = ReadFloatBE(header + 20);
    const unsigned int tableOffset = endian::LoadU32BE(header + 24);
    clip.dataOffset = endian::LoadU32BE(header + 28);
    clip.frameStride = trackCount * animfmt::kSampleBytes;
    if (trackCount == 0 || trackCount > 4096 || clip.frameCount == 0 ||
        tableOffset + trackCount * animfmt::kTrackBytes > clip.size) return false;
    if (clip.frameMajor && (clip.dataOffset > clip.size ||
        clip.frameStride > clip.size - clip.dataOffset ||
        clip.frameCount > (clip.size - clip.dataOffset) / clip.frameStride)) return false;
    std::vector<unsigned char> records(trackCount * animfmt::kTrackBytes);
    if (!m_pak->ReadRawRange(m_entry, clip.offset + tableOffset, &records[0], (unsigned int)records.size())) return false;
    for (unsigned int index = 0; index < trackCount; ++index)
    {
        const unsigned char* record = &records[index * animfmt::kTrackBytes];
        Track track;
        track.nameHash = endian::LoadU32BE(record);
        for (int axis = 0; axis < 3; ++axis) track.translationMin[axis] = ReadFloatBE(record + 4 + axis * 4);
        for (int axis = 0; axis < 3; ++axis) track.translationExtent[axis] = ReadFloatBE(record + 16 + axis * 4);
        for (int axis = 0; axis < 3; ++axis) track.scaleMin[axis] = ReadFloatBE(record + 28 + axis * 4);
        for (int axis = 0; axis < 3; ++axis) track.scaleExtent[axis] = ReadFloatBE(record + 40 + axis * 4);
        track.sampleOffset = endian::LoadU32BE(record + 52);
        if ((!clip.frameMajor && track.sampleOffset + clip.frameCount * animfmt::kSampleBytes > clip.size) ||
            (clip.frameMajor && track.sampleOffset + animfmt::kSampleBytes > clip.frameStride)) return false;
        clip.tracks.push_back(track);
    }
    return true;
}

void RuntimeAnimator::DecodeTransform(const Track& track, const unsigned char* sample,
                                      Transform& output) const
{
    for (int axis = 0; axis < 3; ++axis)
        output.translation[axis] = track.translationMin[axis] + track.translationExtent[axis] *
            ((float)endian::LoadU16BE(sample + axis * 2) / 65535.0f);
    float lengthSquared = 0.0f;
    for (int component = 0; component < 4; ++component)
    {
        output.rotation[component] = (float)(short)endian::LoadU16BE(sample + 6 + component * 2) / 32767.0f;
        lengthSquared += output.rotation[component] * output.rotation[component];
    }
    const float inverseLength = lengthSquared > 1e-12f ? 1.0f / sqrtf(lengthSquared) : 1.0f;
    for (int component = 0; component < 4; ++component) output.rotation[component] *= inverseLength;
    for (int axis = 0; axis < 3; ++axis)
        output.scale[axis] = track.scaleMin[axis] + track.scaleExtent[axis] *
            ((float)endian::LoadU16BE(sample + 14 + axis * 2) / 65535.0f);
}

void RuntimeAnimator::PageForFrame(const Clip& clip, unsigned int frame,
                                   unsigned int& firstFrame, unsigned int& offset,
                                   unsigned int& bytes) const
{
    unsigned int framesPerPage = spak::kStreamPageBytes / clip.frameStride;
    if (framesPerPage == 0) framesPerPage = 1;
    firstFrame = (frame / framesPerPage) * framesPerPage;
    unsigned int frameCount = framesPerPage;
    if (frameCount > clip.frameCount - firstFrame)
        frameCount = clip.frameCount - firstFrame;
    offset = clip.offset + clip.dataOffset + firstFrame * clip.frameStride;
    bytes = frameCount * clip.frameStride;
}

bool RuntimeAnimator::ReadTransform(const Clip& clip, const Track& track,
                                    unsigned int frame, Transform& output)
{
    unsigned char sample[animfmt::kSampleBytes];
    if (!m_pak->ReadRawRange(m_entry, clip.offset + track.sampleOffset + frame * animfmt::kSampleBytes,
                             sample, sizeof(sample))) return false;
    DecodeTransform(track, sample, output);
    return true;
}

bool RuntimeAnimator::SamplePalette(const RtMesh& mesh, const State& state, float time,
                                    std::vector<float>& output, FrameCache& frameCache,
                                    bool applyPhysics, const float objectWorld[16])
{
    Clip* clip = FindClip(state.clipHash);
    if (!clip || !LoadClip(*clip)) return false;
    if (clip->mappedSkeletonFingerprint != mesh.skeletonFingerprint ||
        clip->mappedJointCount != (unsigned int)mesh.joints.size())
    {
        clip->jointTracks.assign(mesh.joints.size(), -1);
        for (size_t jointIndex = 0; jointIndex < mesh.joints.size(); ++jointIndex)
            for (size_t trackIndex = 0; trackIndex < clip->tracks.size(); ++trackIndex)
                if (clip->tracks[trackIndex].nameHash == mesh.joints[jointIndex].nameHash)
                {
                    clip->jointTracks[jointIndex] = (int)trackIndex;
                    break;
                }
        clip->mappedSkeletonFingerprint = mesh.skeletonFingerprint;
        clip->mappedJointCount = (unsigned int)mesh.joints.size();
    }
    float sampleTime = time;
    if (state.flags & animcook::kStateLoop)
        sampleTime = clip->duration > 0.0f ? fmodf(sampleTime, clip->duration) : 0.0f;
    else sampleTime = Clamp(sampleTime, 0.0f, clip->duration);
    const float framePosition = sampleTime * clip->sampleRate;
    unsigned int frame0 = (unsigned int)framePosition;
    if (frame0 >= clip->frameCount) frame0 = clip->frameCount - 1;
    const unsigned int frame1 = frame0 + 1 < clip->frameCount ? frame0 + 1 : frame0;
    const float alpha = framePosition - (float)frame0;
    if (clip->frameMajor)
    {
        bool ready0 = true, ready1 = true;
        if (frameCache.clipHash != clip->idHash || frameCache.frame0 != frame0 ||
            frameCache.frame1 != frame1)
        {
            frameCache.samples0.resize(clip->frameStride);
            frameCache.samples1.resize(clip->frameStride);
            const unsigned int frameOffset0 = clip->offset + clip->dataOffset + frame0 * clip->frameStride;
            const unsigned int frameOffset1 = clip->offset + clip->dataOffset + frame1 * clip->frameStride;
            ready0 = m_streamCache
                ? m_streamCache->GetRawRange(m_entry, frameOffset0, clip->frameStride, &frameCache.samples0[0])
                : m_pak->ReadRawRange(m_entry, frameOffset0, &frameCache.samples0[0], clip->frameStride);
            ready1 = m_streamCache
                ? m_streamCache->GetRawRange(m_entry, frameOffset1, clip->frameStride, &frameCache.samples1[0])
                : m_pak->ReadRawRange(m_entry, frameOffset1, &frameCache.samples1[0], clip->frameStride);
            if (ready0 && ready1)
            {
                frameCache.clipHash = clip->idHash;
                frameCache.frame0 = frame0;
                frameCache.frame1 = frame1;
            }
        }
        if (m_streamCache)
        {
            unsigned int nextFirst = 0, nextOffset = 0, nextBytes = 0;
            PageForFrame(*clip, frame1, nextFirst, nextOffset, nextBytes);
            unsigned int framesPerPage = spak::kStreamPageBytes / clip->frameStride;
            if (framesPerPage == 0) framesPerPage = 1;
            unsigned int nextFrame = nextFirst + framesPerPage;
            if (nextFrame >= clip->frameCount && (state.flags & animcook::kStateLoop)) nextFrame = 0;
            if (nextFrame < clip->frameCount)
            {
                PageForFrame(*clip, nextFrame, nextFirst, nextOffset, nextBytes);
                if (frameCache.prefetchClipHash != clip->idHash ||
                    frameCache.prefetchPageOffset != nextOffset)
                {
                    m_streamCache->GetRawRange(m_entry, nextOffset, nextBytes, NULL);
                    frameCache.prefetchClipHash = clip->idHash;
                    frameCache.prefetchPageOffset = nextOffset;
                }
            }
        }
        if (!ready0 || !ready1) return false;
    }
    m_globalMatrices.resize(mesh.joints.size() * 16);
    output.resize(mesh.joints.size() * 12);
    if (applyPhysics && m_resource &&
        (m_modifierSkeletonFingerprint != mesh.skeletonFingerprint ||
         m_jointModifiers.size() != mesh.joints.size()))
    {
        runtime_bone_modifiers::MapPhysicsModifiers(
            m_resource->modifiers, mesh, m_jointModifiers);
        m_physicsStates.assign(m_resource->modifiers.size(), RuntimeBonePhysicsState());
        m_modifierSkeletonFingerprint = mesh.skeletonFingerprint;
    }
    for (size_t jointIndex = 0; jointIndex < mesh.joints.size(); ++jointIndex)
    {
        const RtJoint& joint = mesh.joints[jointIndex];
        Matrix local;
        memcpy(local.m, joint.bindLocal, sizeof(local.m));
        const int trackIndex = clip->jointTracks[jointIndex];
        if (trackIndex >= 0)
        {
            const Track& track = clip->tracks[(size_t)trackIndex];
            Transform first, second, blended;
            if (clip->frameMajor)
            {
                DecodeTransform(track, &frameCache.samples0[track.sampleOffset], first);
                DecodeTransform(track, &frameCache.samples1[track.sampleOffset], second);
            }
            else if (!ReadTransform(*clip, track, frame0, first) ||
                     !ReadTransform(*clip, track, frame1, second)) return false;
            for (int axis = 0; axis < 3; ++axis)
            {
                blended.translation[axis] = first.translation[axis] + (second.translation[axis] - first.translation[axis]) * alpha;
                blended.scale[axis] = first.scale[axis] + (second.scale[axis] - first.scale[axis]) * alpha;
            }
            InterpolateQuaternion(first.rotation, second.rotation, alpha, blended.rotation);
            local = Compose(blended);
        }
        Matrix global = local;
        if (joint.parent >= 0)
        {
            Matrix parentGlobal;
            memcpy(parentGlobal.m, &m_globalMatrices[(size_t)joint.parent * 16], sizeof(parentGlobal.m));
            global = MultiplyMatrix(parentGlobal, local);
        }
        Matrix descendantGlobal = global;
        const int modifierIndex = applyPhysics && jointIndex < m_jointModifiers.size()
            ? m_jointModifiers[jointIndex] : -1;
        if (modifierIndex >= 0 && joint.parent >= 0)
        {
            const RuntimeBoneModifier& modifier = m_resource->modifiers[(size_t)modifierIndex];
            RuntimeBonePhysicsState& simulation = m_physicsStates[(size_t)modifierIndex];
            Matrix parentGlobal;
            memcpy(parentGlobal.m, &m_globalMatrices[(size_t)joint.parent * 16], sizeof(parentGlobal.m));
            runtime_bone_modifiers::ApplyPhysics(modifier, m_lastDeltaTime,
                &parentGlobal.m[0][0], objectWorld, simulation, &global.m[0][0]);
            if (modifier.flags & animcook::kModifierAffectsChildren) descendantGlobal = global;
        }
        memcpy(&m_globalMatrices[jointIndex * 16], descendantGlobal.m, sizeof(descendantGlobal.m));
        Matrix inverseBind;
        memcpy(inverseBind.m, joint.inverseBind, sizeof(inverseBind.m));
        const Matrix skin = MultiplyMatrix(global, inverseBind);
        float* palette = &output[jointIndex * 12];
        for (int row = 0; row < 3; ++row)
            for (int column = 0; column < 4; ++column)
                palette[row * 4 + column] = skin.m[row][column];
    }
    return true;
}

bool RuntimeAnimator::BuildPalette(const RtMesh& mesh, const float objectWorld[16],
                                   std::vector<float>& output)
{
    const State* active = FindState(m_activeState);
    bool success = active && SamplePalette(mesh, *active, m_stateTime, m_activePalette,
                                           m_activeFrames, true, objectWorld);
    if (success) m_collisionMatrices = m_globalMatrices;
    const State* previous = FindState(m_previousState);
    const bool blending = success && previous && m_blendDuration > 0.0f;
    if (blending)
    {
        if (SamplePalette(mesh, *previous, m_previousTime, m_previousPalette,
                  m_previousFrames, false, objectWorld) &&
            m_previousPalette.size() == m_activePalette.size())
        {
            const float alpha = 1.0f - Clamp(m_blendRemaining / m_blendDuration, 0.0f, 1.0f);
            for (size_t index = 0; index < m_activePalette.size(); ++index)
                m_activePalette[index] = m_previousPalette[index] +
                    (m_activePalette[index] - m_previousPalette[index]) * alpha;
        }
        else success = false;
    }
    if (success) output.swap(m_activePalette);
    return success;
}

void RuntimeAnimator::GetBoneColliders(const RtMesh& mesh,
                                       std::vector<BoneColliderPose>& output) const
{
    output.clear();
    if (!m_resource || m_collisionMatrices.size() != mesh.joints.size() * 16) return;
    for (size_t modifierIndex = 0; modifierIndex < m_resource->modifiers.size(); ++modifierIndex)
    {
        const RuntimeBoneModifier& modifier = m_resource->modifiers[modifierIndex];
        if (modifier.type != animcook::ModifierCollision) continue;
        size_t jointIndex = mesh.joints.size();
        for (size_t candidate = 0; candidate < mesh.joints.size(); ++candidate)
            if (mesh.joints[candidate].nameHash == modifier.boneHash)
            { jointIndex = candidate; break; }
        if (jointIndex == mesh.joints.size()) continue;
        const float* columnMatrix = &m_collisionMatrices[jointIndex * 16];
        BoneColliderPose pose;
        pose.boneHash = modifier.boneHash;
        for (int axis = 0; axis < 3; ++axis) pose.halfExtents[axis] = modifier.boxHalfExtents[axis];
        for (int row = 0; row < 4; ++row)
            for (int column = 0; column < 4; ++column)
                pose.modelMatrix[row * 4 + column] = columnMatrix[column * 4 + row];
        pose.modelMatrix[12] += modifier.boxCenter[0] * pose.modelMatrix[0] +
                                modifier.boxCenter[1] * pose.modelMatrix[4] +
                                modifier.boxCenter[2] * pose.modelMatrix[8];
        pose.modelMatrix[13] += modifier.boxCenter[0] * pose.modelMatrix[1] +
                                modifier.boxCenter[1] * pose.modelMatrix[5] +
                                modifier.boxCenter[2] * pose.modelMatrix[9];
        pose.modelMatrix[14] += modifier.boxCenter[0] * pose.modelMatrix[2] +
                                modifier.boxCenter[1] * pose.modelMatrix[6] +
                                modifier.boxCenter[2] * pose.modelMatrix[10];
        output.push_back(pose);
    }
}

void RuntimeAnimator::SetFloat(const char* name, float value)
{
    const unsigned int hash = spak::NameHash(name);
    m_floatParameters[hash] = value;
    m_boolParameters.erase(hash);
}

void RuntimeAnimator::SetBool(const char* name, bool value)
{
    const unsigned int hash = spak::NameHash(name);
    m_boolParameters[hash] = value;
    m_floatParameters.erase(hash);
}

void RuntimeAnimator::SetTrigger(const char* name)
{
    m_triggers.insert(spak::NameHash(name));
}

void RuntimeAnimator::SetState(const char* name)
{
    const unsigned int hash = spak::NameHash(name);
    if (!FindState(hash)) return;
    m_activeState = hash;
    m_previousState = 0;
    m_stateTime = m_previousTime = m_blendDuration = m_blendRemaining = 0.0f;
}

void RuntimeAnimator::ClearSharedResources()
{
    for (std::map<const SpakEntry*, Resource*>::iterator it = s_resources.begin();
         it != s_resources.end(); ++it)
        delete it->second;
    s_resources.clear();
}

const char* RuntimeAnimator::BoneName(unsigned int hash) const
{
    if (!m_resource) return NULL;
    for (size_t index = 0; index < m_resource->modifiers.size(); ++index)
        if (m_resource->modifiers[index].boneHash == hash)
            return m_resource->modifiers[index].boneName.c_str();
    return NULL;
}